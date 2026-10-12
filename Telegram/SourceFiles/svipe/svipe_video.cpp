/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_video.h"

#include "base/call_delayed.h"
#include "base/timer_rpl.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_strings.h"
#include "lang/lang_keys.h"
#include "svipe/svipe_storage.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUrl>

namespace Svipe::Video {
namespace {

// Android: SvipeExploreGrid.MAX_PIPE_FAILURES and the cold-start retry spacing.
constexpr auto kMaxFailures = 2;
constexpr auto kRetryDelay = crl::time(3000);

// Android: SvipeVideoTelemetry — a heartbeat every 30 s while playing, never two within 5 s.
constexpr auto kHeartbeatPeriod = crl::time(30000);
constexpr auto kHeartbeatMinGap = crl::time(5000);
// Android: SvipeLongWatch thresholds.
constexpr auto kEndedFraction = 0.98;
constexpr auto kMeaningfulWatch = crl::time(10000);

} // namespace

List::List(not_null<Reels::Feed*> feed, QString path, int pageSize)
: _feed(feed)
, _path(std::move(path))
, _pageSize(pageSize) {
}

void List::loadMore() {
	if (_loading || _exhausted) {
		return;
	}
	_loading = true;
	const auto startedAt = crl::now();
	LOG(("Svipe Video: %1 page from %2 (generation %3)").arg(_path).arg(_offset).arg(_generation));
	if (_items.empty()) {
		_status = Tr(Str::ReelsLoadingFeed);
		_updates.fire({});
	}
	const auto session = _feed->session();
	const auto weak = base::make_weak(this);
	const auto generation = _generation;
	Auth::EnsureToken(session, [=](QString token) {
		if (!weak || generation != _generation) {
			return;
		}
		const auto retry = [=] {
			_loading = false;
			if (++_failures >= kMaxFailures && !_items.empty()) {
				_exhausted = true; // a dead pipe stops asking; what it served stays
				_updates.fire({});
				return;
			}
			base::call_delayed(kRetryDelay * _failures, session, [=] {
				if (weak) {
					loadMore();
				}
			});
		};
		if (token.isEmpty()) {
			if (_items.empty()) {
				_status = Tr(Str::ReelsConnectFailed);
				_updates.fire({});
			}
			retry();
			return;
		}
		LOG(("Svipe Video: token after %1 ms (%2)").arg(crl::now() - startedAt).arg(token.isEmpty() ? "none" : "ok"));
		const auto separator = _path.contains('?') ? '&' : '?';
		const auto path = _path + separator
			+ u"limit=%1&offset=%2"_q.arg(_pageSize).arg(_offset);
		Api::Get(path, token, [=](QJsonObject result, int code) {
			LOG(("Svipe Video: %1 -> %2 in %3 ms, %4 items, current %5"
				).arg(path
				).arg(code
				).arg(crl::now() - startedAt
				).arg(result.value(u"items"_q).toArray().size()
				).arg(Logs::b(weak && generation == _generation)));
			if (!weak || generation != _generation) {
				return;
			}
			_answered = true;
			if (!result.contains(u"items"_q)) {
				if (_items.empty()) {
					_status = !code
						? Tr(Str::ReelsNoInternet)
						: Tr(Str::ReelsLoadFailed).arg(code);
					_updates.fire({});
				}
				retry();
				return;
			}
			_loading = false;
			_failures = 0;
			const auto recId = result.value(u"recommendation_id"_q).toString();
			const auto recAt = crl::now();
			for (const auto &value : result.value(u"items"_q).toArray()) {
				auto item = Reels::Feed::ParseItem(value.toObject());
				if (!item) {
					continue;
				}
				const auto duplicate = ranges::any_of(_items, [&](const auto &i) {
					return i->channelId == item->channelId
						&& i->messageId == item->messageId;
				});
				if (duplicate) {
					continue;
				}
				item->recId = recId;
				item->recAt = recAt;
				item->shortForm = _shortForm;
				item->feedPosition = int(_items.size());
				_items.push_back(std::move(item));
			}
			const auto next = result.value(u"next_offset"_q);
			if (next.isDouble()) {
				_offset = next.toInt();
			} else {
				_exhausted = true;
			}
			_status = (_items.empty() && _exhausted)
				? Tr(Str::ReelsEmpty)
				: QString();
			_updates.fire({});
		});
	});
}

void List::reset() {
	++_generation;
	_answered = false;
	_items.clear();
	_offset = 0;
	_failures = 0;
	_loading = false;
	_exhausted = false;
	_status = QString();
	_updates.fire({});
	loadMore();
}

void List::remove(const std::shared_ptr<Item> &item) {
	const auto i = ranges::find(_items, item);
	if (i != end(_items)) {
		_items.erase(i);
		_updates.fire({});
	}
}

// ---- Search -------------------------------------------------------------------------------

namespace {

// Android: SvipeDiscover.PAGE_SIZE / LONG_PAGE_SIZE for the two search pipes.
constexpr auto kShortsSearchPage = 60;
constexpr auto kLongsSearchPage = 20;
// Android: SvipeVideoSearchHistory.MAX.
constexpr auto kHistoryMax = 20;
const auto kHistoryKey = u"svipe_video_search_history"_q;

[[nodiscard]] QString Encoded(const QString &query) {
	return QString::fromUtf8(QUrl::toPercentEncoding(query));
}

[[nodiscard]] QJsonObject Serialize(const Item &item) {
	return QJsonObject{
		{ u"channel_id"_q, QString::number(item.channelId) },
		{ u"message_id"_q, qint64(item.messageId.bare) },
		{ u"username"_q, item.username },
		{ u"topic_id"_q, item.topicId },
		{ u"width"_q, item.width },
		{ u"height"_q, item.height },
		{ u"duration_ms"_q, qint64(item.durationMs) },
		{ u"share_url"_q, item.shareUrl },
		{ u"short"_q, item.shortForm },
	};
}

} // namespace

SearchSource::SearchSource(not_null<Reels::Feed*> feed, const QString &query)
: _query(query)
, _shorts(feed, u"/v1/discover/search?q="_q + Encoded(query), kShortsSearchPage)
, _longs(feed, u"/v1/videos/search?q="_q + Encoded(query), kLongsSearchPage) {
	_shorts.markShortForm();
	rpl::merge(
		_shorts.updates(),
		_longs.updates()
	) | rpl::on_next([=] {
		merge();
	}, _lifetime);
	_shorts.loadMore();
	_longs.loadMore();
}

bool SearchSource::exhausted() const {
	return _shorts.exhausted() && (_longs.exhausted() || _longs.failedOut());
}

QString SearchSource::status() const {
	if (!_merged.empty()) {
		return QString();
	}
	if (_shorts.failedOut() && (_longs.failedOut() || _longs.items().empty())) {
		return _shorts.status(); // "No internet…" / "Couldn't load…": never a spinner for ever
	}
	const auto settled = _shorts.answered() && _longs.answered();
	return (settled && exhausted())
		? tr::lng_search_tab_no_results(tr::now)
		: tr::lng_contacts_loading(tr::now);
}

void SearchSource::merge() {
	// The first screen waits for both pipes, as on Android: a page of reels alone would be
	// re-laid the moment the long videos land.
	const auto longsIn = _longs.answered() || _longs.failedOut();
	if (!_shorts.answered() || !longsIn) {
		_updates.fire({});
		return;
	}
	const auto &longs = _longs.items();
	const auto &shorts = _shorts.items();
	auto merged = std::vector<std::shared_ptr<Item>>();
	merged.reserve(longs.size() + shorts.size());
	auto i = 0, j = 0;
	const auto wide = std::max(_wide, 1), tall = std::max(_tall, 1);
	while (i < int(longs.size()) || j < int(shorts.size())) {
		for (auto k = 0; k != wide && i < int(longs.size()); ++k) {
			merged.push_back(longs[i++]);
		}
		for (auto k = 0; k != tall && j < int(shorts.size()); ++k) {
			merged.push_back(shorts[j++]);
		}
	}
	_merged = std::move(merged);
	_updates.fire({});
}

void SearchSource::loadMore() {
	_shorts.loadMore();
	if (!_longs.failedOut()) {
		_longs.loadMore();
	}
}

void SearchSource::remove(const std::shared_ptr<Item> &item) {
	(item->shortForm ? _shorts : _longs).remove(item);
}

void SearchSource::setRowSizes(int wide, int tall) {
	if (_wide != wide || _tall != tall) {
		_wide = wide;
		_tall = tall;
		merge();
	}
}

RecentSource::RecentSource(not_null<Main::Session*> session)
: _session(session) {
	for (const auto &value : Storage::Get(session, kHistoryKey).toArray()) {
		const auto o = value.toObject();
		if (auto item = Reels::Feed::ParseItem(o)) {
			item->shortForm = o.value(u"short"_q).toBool();
			_items.push_back(std::move(item));
		}
	}
}

void RecentSource::remove(const std::shared_ptr<Item> &item) {
	auto list = QJsonArray();
	for (const auto &value : Storage::Get(_session, kHistoryKey).toArray()) {
		const auto o = value.toObject();
		const auto same = o.value(u"channel_id"_q).toVariant().toULongLong() == item->channelId
			&& o.value(u"message_id"_q).toInteger() == item->messageId.bare;
		if (!same) {
			list.push_back(o);
		}
	}
	Storage::Set(_session, kHistoryKey, list);
	_items.erase(ranges::remove(_items, item), end(_items));
	_updates.fire({});
}

void RecentSource::clear() {
	Storage::Set(_session, kHistoryKey, QJsonArray());
	_items.clear();
	_updates.fire({});
}

namespace SearchHistory {

bool Empty(not_null<Main::Session*> session) {
	return Storage::Get(session, kHistoryKey).toArray().isEmpty();
}

void Add(not_null<Main::Session*> session, const Item &item) {
	auto list = QJsonArray{ Serialize(item) };
	for (const auto &value : Storage::Get(session, kHistoryKey).toArray()) {
		const auto o = value.toObject();
		const auto same = o.value(u"channel_id"_q).toVariant().toULongLong() == item.channelId
			&& o.value(u"message_id"_q).toInteger() == item.messageId.bare;
		if (!same && list.size() < kHistoryMax) {
			list.push_back(o);
		}
	}
	Storage::Set(session, kHistoryKey, list);
}

} // namespace SearchHistory

// ---- Watch telemetry ----------------------------------------------------------------------

Watch::Watch(not_null<Reels::Feed*> feed, std::shared_ptr<Item> item)
: _feed(feed)
, _item(std::move(item))
, _openedAt(crl::now()) {
	_duration = _item->durationMs;
	send(u"IMPRESSION"_q);
	base::timer_each(
		kHeartbeatPeriod
	) | rpl::on_next([=] {
		if (_playStartedAt) {
			heartbeat();
		}
	}, _heartbeatLifetime);
}

Watch::~Watch() {
	const auto now = crl::now();
	if (_playStartedAt) {
		_watched += now - _playStartedAt;
		_playStartedAt = 0;
	}
	if (_bufferingSince) {
		_buffering += now - _bufferingSince;
		_bufferingSince = 0;
	}
	// SvipeLongWatch.classify: finished, or watched enough to mean something, or stalled at least
	// as long as it played (the network, not the viewer, ended it) — otherwise a swipe away.
	const auto firstFrameWait = (_firstFrameAfter >= 0)
		? _firstFrameAfter
		: (now - _openedAt);
	const auto reachedEnd = _ended
		|| (_duration > 0 && _position >= kEndedFraction * _duration);
	const auto type = reachedEnd
		? u"VIDEO_END"_q
		: (_watched >= kMeaningfulWatch
			|| (_buffering + firstFrameWait) >= _watched)
		? u"HEARTBEAT"_q
		: u"SWIPE_AWAY"_q;
	auto extra = QJsonObject{ { u"dwell_ms"_q, qint64(now - _openedAt) } };
	if (_firstFrameAfter >= 0) {
		extra.insert(u"time_to_first_frame_ms"_q, qint64(_firstFrameAfter));
	}
	send(type, extra);
}

QJsonObject Watch::payload() const {
	const auto now = crl::now();
	auto result = QJsonObject{
		{ u"watched_ms"_q, qint64(_watched
			+ (_playStartedAt ? (now - _playStartedAt) : 0)) },
		{ u"position_ms"_q, qint64(_position) },
		{ u"buffering_ms"_q, qint64(_buffering
			+ (_bufferingSince ? (now - _bufferingSince) : 0)) },
		{ u"autoplay"_q, false },
		{ u"network_type"_q, u"wifi"_q },
	};
	if (_duration > 0) {
		result.insert(u"video_duration_ms"_q, qint64(_duration));
	}
	return result;
}

void Watch::send(const QString &type, QJsonObject extra) {
	auto body = payload();
	for (auto i = extra.begin(); i != extra.end(); ++i) {
		body.insert(i.key(), i.value());
	}
	_feed->sendEvent(*_item, type, body);
}

void Watch::heartbeat() {
	const auto now = crl::now();
	if (_lastHeartbeat && now - _lastHeartbeat < kHeartbeatMinGap) {
		return;
	}
	_lastHeartbeat = now;
	send(u"HEARTBEAT"_q);
}

void Watch::playing(bool playing, crl::time position, crl::time duration) {
	if (position >= 0) {
		_position = position;
	}
	if (duration > 0) {
		_duration = duration;
	}
	const auto now = crl::now();
	if (playing && !_playStartedAt) {
		_playStartedAt = now;
		if (!_playStartSent) {
			_playStartSent = true;
			send(u"PLAY_START"_q);
		}
	} else if (!playing && _playStartedAt) {
		_watched += now - _playStartedAt;
		_playStartedAt = 0;
		heartbeat(); // a pause is a checkpoint, as on Android
	}
}

void Watch::firstFrame() {
	if (_firstFrameAfter >= 0) {
		return;
	}
	_firstFrameAfter = crl::now() - _openedAt;
	send(u"FIRST_FRAME"_q, QJsonObject{
		{ u"time_to_first_frame_ms"_q, qint64(_firstFrameAfter) },
	});
}

void Watch::buffering(bool buffering) {
	const auto now = crl::now();
	if (buffering && !_bufferingSince) {
		_bufferingSince = now;
	} else if (!buffering && _bufferingSince) {
		_buffering += now - _bufferingSince;
		_bufferingSince = 0;
	}
}

void Watch::ended() {
	_ended = true;
}

} // namespace Svipe::Video
