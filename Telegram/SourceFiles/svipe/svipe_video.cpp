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

#include <QtCore/QJsonArray>

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
