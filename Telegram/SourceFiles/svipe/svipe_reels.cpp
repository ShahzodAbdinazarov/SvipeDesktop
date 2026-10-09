/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_reels.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "history/history.h"
#include "base/timer_rpl.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_histories.h"
#include "api/api_common.h"
#include "api/api_sending.h"
#include "data/data_web_page.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUrlQuery>

namespace Svipe::Reels {
namespace {

// Android: SvipeRecAttribution.DEFAULT_TTL_SECONDS / SAFETY_MARGIN_MS.
constexpr auto kDefaultRecTtl = crl::time(3600 * 1000);
constexpr auto kRecSafetyMargin = crl::time(60 * 1000);

// Android: ReelsActivity.MAX_EMPTY_APPEND_PAGES and the cold-start backoff (3 s -> 30 s).
constexpr auto kMaxEmptyPages = 25;
constexpr auto kRetryFirst = crl::time(3000);
constexpr auto kRetryMax = crl::time(30000);

// Android: SvipeChannelResolve — one resolveUsername in flight, 1200 ms apart, 150 an hour, a
// FLOOD_WAIT honoured (capped at a day) and remembered across launches.
constexpr auto kUsernameSpacing = crl::time(1200);
constexpr auto kUsernameBudget = 150;
constexpr auto kUsernameWindow = 3600;
constexpr auto kMaxBlock = 86400;

// Android: SvipeRefResolver's getWebPage retry: previews are generated asynchronously.
constexpr auto kWebPageRetry = crl::time(1500);
constexpr auto kWebPageTimeout = crl::time(8000);

// Android: SvipeWatchedSet cap.
constexpr auto kMaxWatched = 2000;
constexpr auto kMaxHashes = 4000;

const auto kWatchedKey = u"svipe_reel_watched"_q;
const auto kBlockedKey = u"svipe_reel_blocked_channels"_q;
const auto kHashesKey = u"svipe_reel_channel_hashes"_q;
const auto kSpentKey = u"svipe_resolve_spent"_q;
const auto kWindowKey = u"svipe_resolve_window"_q;
const auto kBlockedUntilKey = u"svipe_resolve_blocked_until"_q;
const auto kSavedKey = u"svipe_saved_reels_channel"_q;

// Every name the phone may have given the channel (Android: SvipeSavedReelsChannel).
// The tab was Clips before it was Lavha: a channel made under the old name is still ours, and is
// renamed the first time it is found (Android does the same, so both devices keep one channel).
[[nodiscard]] QStringList SavedClipsOldTitles() {
	return {
		u"Saved Clips"_q,
		u"Saqlangan Clips"_q,
		QString::fromUtf8("Сохранённые Clips"),
	};
}

[[nodiscard]] QStringList SavedClipsTitles() {
	return QStringList{
		u"Saved Lavha"_q,
		u"Saqlangan lavhalar"_q,
		QString::fromUtf8("Сохранённые Lavha"),
	} + SavedClipsOldTitles();
}

[[nodiscard]] QString Key(uint64 channelId, MsgId messageId) {
	return QString::number(channelId) + ':' + QString::number(messageId.bare);
}

[[nodiscard]] QString PostUrl(const Item &item) {
	return u"https://t.me/%1/%2"_q.arg(item.username).arg(item.messageId.bare);
}

[[nodiscard]] crl::time &RecTtl() {
	static auto result = kDefaultRecTtl;
	return result;
}

[[nodiscard]] int FloodSeconds(const MTP::Error &error) {
	const auto &type = error.type();
	for (const auto &prefix : {
			u"FLOOD_WAIT_"_q,
			u"FLOOD_PREMIUM_WAIT_"_q,
			u"SLOWMODE_WAIT_"_q }) {
		if (type.startsWith(prefix)) {
			return type.mid(prefix.size()).toInt();
		}
	}
	return 0;
}

[[nodiscard]] bool IsVideo(DocumentData *document) {
	return document
		&& (document->isVideoFile() || document->isAnimation())
		&& !document->isVideoMessage();
}

} // namespace

Feed::Feed(not_null<Main::Session*> session)
: _session(session) {
}

Feed::~Feed() = default;

QString Feed::Classify(crl::time watched, crl::time duration) {
	if (duration > 0) {
		if (watched >= 1.5 * duration) {
			return u"REPLAY"_q;
		} else if (watched >= 0.9 * duration) {
			return u"VIDEO_END"_q;
		}
	}
	return u"SWIPE_AWAY"_q;
}

std::shared_ptr<Item> Feed::ParseItem(const QJsonObject &o) {
	const auto username = o.value(u"username"_q).toString();
	const auto channelId = uint64(o.value(u"channel_id"_q).toVariant().toULongLong());
	const auto messageId = MsgId(o.value(u"message_id"_q).toInt());
	if (username.isEmpty() || !channelId || !messageId) {
		return nullptr;
	}
	auto item = std::make_shared<Item>();
	item->channelId = channelId;
	item->messageId = messageId;
	item->username = username;
	item->shareUrl = o.value(u"share_url"_q).toString();
	item->width = o.value(u"width"_q).toInt();
	item->height = o.value(u"height"_q).toInt();
	item->durationMs = o.value(u"duration_ms"_q).toInteger();
	const auto thumb = o.value(u"thumb_b64"_q).toString();
	if (!thumb.isEmpty()) {
		item->inlineThumb = QByteArray::fromBase64(thumb.toLatin1());
	}
	return item;
}

bool Feed::isWatched(uint64 channelId, MsgId messageId) const {
	const auto key = Key(channelId, messageId);
	for (const auto &value : Storage::Get(_session, kWatchedKey).toArray()) {
		if (value.toString() == key) {
			return true;
		}
	}
	return false;
}

bool Feed::isBlocked(uint64 channelId) const {
	for (const auto &value : Storage::Get(_session, kBlockedKey).toArray()) {
		if (uint64(value.toVariant().toULongLong()) == channelId) {
			return true;
		}
	}
	return false;
}

bool Feed::contains(uint64 channelId, MsgId messageId) const {
	return ranges::any_of(_items, [&](const std::shared_ptr<Item> &item) {
		return (item->channelId == channelId)
			&& (item->messageId == messageId);
	});
}

void Feed::markWatched(const Item &item) {
	auto list = Storage::Get(_session, kWatchedKey).toArray();
	const auto key = Key(item.channelId, item.messageId);
	for (const auto &value : list) {
		if (value.toString() == key) {
			return;
		}
	}
	list.push_back(key);
	while (list.size() > kMaxWatched) {
		list.removeFirst();
	}
	Storage::Set(_session, kWatchedKey, list);
}

void Feed::loadMore() {
	if (_loading || _exhausted) {
		return;
	}
	request();
}

void Feed::request() {
	_loading = true;
	if (_items.empty()) {
		_status = Tr(Str::ReelsLoadingFeed);
		_updates.fire({});
	}
	const auto weak = base::make_weak(this);
	Auth::EnsureToken(_session, [=](QString token) {
		if (!weak) {
			return;
		} else if (token.isEmpty()) {
			_loading = false;
			if (_items.empty()) {
				_status = Tr(Str::ReelsConnectFailed);
				_updates.fire({});
			}
			scheduleRetry();
			return;
		}
		auto path = u"/v1/feed"_q;
		if (!_cursor.isEmpty()) {
			path += u"?cursor="_q + QString::fromUtf8(QUrl::toPercentEncoding(_cursor));
		}
		Api::Get(path, token, [=](QJsonObject result, int code) {
			if (!weak) {
				return;
			}
			_loading = false;
			if (!result.contains(u"items"_q)) {
				if (_items.empty()) {
					_status = !code
						? Tr(Str::ReelsNoInternet)
						: Tr(Str::ReelsLoadFailed).arg(code);
					_updates.fire({});
				}
				scheduleRetry();
				return;
			}
			_failures = 0;
			LOG(("Svipe Reels: feed page, %1 items"
				).arg(result.value(u"items"_q).toArray().size()));
			const auto recId = result.value(u"recommendation_id"_q).toString();
			if (const auto ttl = result.value(
					u"recommendation_ttl_seconds"_q).toInteger(); ttl > 0) {
				RecTtl() = crl::time(ttl) * 1000;
			}
			const auto recAt = crl::now();
			const auto next = result.value(u"next_cursor"_q);
			_cursor = next.isString() ? next.toString() : QString();
			auto added = 0;
			for (const auto &value : result.value(u"items"_q).toArray()) {
				const auto o = value.toObject();
				const auto username = o.value(u"username"_q).toString();
				if (username.isEmpty()) {
					continue;
				}
				const auto channelId = uint64(
					o.value(u"channel_id"_q).toVariant().toULongLong());
				const auto messageId = MsgId(o.value(u"message_id"_q).toInt());
				if (!channelId
					|| !messageId
					|| contains(channelId, messageId)
					|| isWatched(channelId, messageId)
					|| isBlocked(channelId)) {
					continue;
				}
				auto item = std::make_shared<Item>();
				item->channelId = channelId;
				item->messageId = messageId;
				item->username = username;
				item->shareUrl = o.value(u"share_url"_q).toString();
				item->width = o.value(u"width"_q).toInt();
				item->height = o.value(u"height"_q).toInt();
				item->durationMs = o.value(u"duration_ms"_q).toInteger();
				item->recId = recId;
				item->recAt = recAt;
				item->feedPosition = int(_items.size());
				_items.push_back(std::move(item));
				++added;
			}
			if (added) {
				_emptyPages = 0;
			} else if (++_emptyPages >= kMaxEmptyPages) {
				_exhausted = true;
			}
			if (_cursor.isEmpty()) {
				_exhausted = true;
			}
			_status = (_items.empty() && _exhausted)
				? Tr(Str::ReelsEmpty)
				: QString();
			_updates.fire({});
			// A page that added nothing (all watched) is no reason to stop: the next one may.
			if (!added && !_exhausted) {
				request();
			}
		});
	});
}

void Feed::scheduleRetry() {
	const auto delay = std::min(kRetryFirst << std::min(_failures, 4), kRetryMax);
	++_failures;
	const auto weak = base::make_weak(this);
	base::call_delayed(delay, _session, [=] {
		if (weak) {
			loadMore();
		}
	});
}

ChannelData *Feed::knownChannel(uint64 channelId) {
	const auto id = ChannelId(channelId);
	if (const auto channel = _session->data().channelLoaded(id)) {
		if (channel->accessHash()) {
			return channel;
		}
	}
	const auto hashes = Storage::Get(_session, kHashesKey).toObject();
	const auto stored = hashes.value(QString::number(channelId)).toString();
	if (const auto hash = stored.toULongLong()) {
		const auto channel = _session->data().channel(id);
		if (!channel->accessHash()) {
			channel->setAccessHash(hash);
		}
		return channel;
	}
	return nullptr;
}

void Feed::rememberChannel(not_null<ChannelData*> channel) {
	const auto hash = channel->accessHash();
	if (!hash) {
		return;
	}
	auto hashes = Storage::Get(_session, kHashesKey).toObject();
	const auto key = QString::number(peerToChannel(channel->id).bare);
	const auto value = QString::number(hash);
	if (hashes.value(key).toString() == value) {
		return;
	}
	if (hashes.size() >= kMaxHashes) {
		hashes.erase(hashes.begin());
	}
	hashes.insert(key, value);
	Storage::Set(_session, kHashesKey, hashes);
}

bool Feed::takeMessage(
		const std::shared_ptr<Item> &item,
		HistoryItem *message) {
	const auto media = message ? message->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!IsVideo(document)) {
		return false;
	}
	item->document = document;
	item->origin = Data::FileOrigin(message->fullId());
	item->fullId = message->fullId();
	item->title = message->history()->peer->name();
	item->caption = message->originalText();
	if (const auto channel = message->history()->peer->asChannel()) {
		rememberChannel(channel);
	}
	return true;
}

void Feed::resolve(
		const std::shared_ptr<Item> &item,
		bool urgent,
		Fn<void()> done,
		bool background) {
	if (item->document || item->failed) {
		if (done) {
			done();
		}
		return;
	}
	if (done) {
		item->waiters.push_back(std::move(done));
	}
	if (item->resolving) {
		return;
	}
	item->resolving = true;
	const auto weak = base::make_weak(this);
	tryMessage(item, [=](bool ok) {
		if (!weak) {
			return;
		} else if (ok) {
			finish(item, true);
			return;
		}
		tryWebPage(item, [=](bool ok) {
			if (!weak) {
				return;
			} else if (ok || urgent || background) {
				// No resolveUsername for playback, ever: what a preview cannot give is dropped.
				finish(item, ok);
				return;
			}
			// Read-ahead gets another chance on screen.
			item->resolving = false;
			for (const auto &waiter : base::take(item->waiters)) {
				waiter();
			}
		});
	});
}

void Feed::finish(const std::shared_ptr<Item> &item, bool ok) {
	item->resolving = false;
	item->failed = !ok;
	for (const auto &waiter : base::take(item->waiters)) {
		waiter();
	}
	_updates.fire({});
}

void Feed::tryMessage(
		const std::shared_ptr<Item> &item,
		Fn<void(bool)> done) {
	const auto channel = knownChannel(item->channelId);
	if (!channel) {
		LOG(("Svipe Reels: %1 no known channel").arg(PostUrl(*item)));
		done(false);
		return;
	}
	if (const auto existing = _session->data().message(
			channel->id,
			item->messageId)) {
		done(takeMessage(item, existing));
		return;
	}
	const auto weak = base::make_weak(this);
	_session->api().requestMessageData(channel, item->messageId, [=] {
		if (!weak) {
			return;
		}
		const auto message = _session->data().message(
			channel->id,
			item->messageId);
		const auto ok = takeMessage(item, message);
		LOG(("Svipe Reels: %1 getMessages -> message %2, video %3"
			).arg(PostUrl(*item)
			).arg(Logs::b(message != nullptr)
			).arg(Logs::b(ok)));
		done(ok);
	});
}

void Feed::tryWebPage(
		const std::shared_ptr<Item> &item,
		Fn<void(bool)> done,
		bool retried) {
	const auto url = PostUrl(*item);
	const auto weak = base::make_weak(this);
	// Android: SvipeWebRef's STEP_TIMEOUT. A preview Telegram cannot generate can stay unanswered;
	// past this the next route is tried instead of waiting for ever.
	const auto answered = std::make_shared<bool>(false);
	const auto once = [=](bool ok) {
		if (!*answered) {
			*answered = true;
			done(ok);
		}
	};
	base::call_delayed(kWebPageTimeout, _session, [=] {
		if (weak && !*answered) {
			LOG(("Svipe Reels: %1 getWebPage timed out").arg(url));
			once(false);
		}
	});
	_session->api().request(MTPmessages_GetWebPage(
		MTP_string(url),
		MTP_int(0)
	)).done([=](const MTPmessages_WebPage &result) {
		if (!weak) {
			return;
		}
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		if (const auto channel = _session->data().channelLoaded(
				ChannelId(item->channelId))) {
			rememberChannel(channel);
		}
		auto pending = false;
		data.vwebpage().match([&](const MTPDwebPagePending &) {
			pending = true;
		}, [](const auto &) {});
		const auto page = _session->data().processWebpage(data.vwebpage());
		LOG(("Svipe Reels: %1 getWebPage -> pending %2, type %3, document %4, video %5"
			).arg(url
			).arg(Logs::b(pending)
			).arg(int(page->type)
			).arg(Logs::b(page->document != nullptr)
			).arg(Logs::b(IsVideo(page->document))));
		if (IsVideo(page->document)) {
			item->document = page->document;
			item->origin = Data::FileOriginWebPage{ url };
			// A post preview's title is the channel's name, its description the post's text.
			item->title = page->title.isEmpty() ? page->siteName : page->title;
			item->caption = page->description;
			once(true);
		} else if (pending && !retried) {
			base::call_delayed(kWebPageRetry, _session, [=] {
				if (weak) {
					if (!*answered) { *answered = true; tryWebPage(item, done, true); }
				}
			});
		} else {
			once(false);
		}
	}).fail([=](const MTP::Error &error) {
		LOG(("Svipe Reels: %1 getWebPage failed: %2").arg(url, error.type()));
		if (weak) {
			once(false);
		}
	}).handleFloodErrors().send();
}

void Feed::tryUsername(
		const std::shared_ptr<Item> &item,
		Fn<void(bool)> done,
		bool back) {
	const auto now = base::unixtime::now();
	if (Storage::Get(_session, kBlockedUntilKey).toInteger() > now) {
		done(false);
		return;
	}
	if (back) {
		_usernameQueue.push_back({ item, std::move(done) });
	} else {
		_usernameQueue.push_front({ item, std::move(done) });
	}
	pumpUsernames();
}

void Feed::pumpUsernames() {
	if (_usernameInFlight || _usernameQueue.empty()) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto wait = _usernameLastAt
		? (_usernameLastAt + kUsernameSpacing - crl::now())
		: crl::time(0);
	if (wait > 0) {
		base::call_delayed(wait, _session, [=] {
			if (weak) {
				pumpUsernames();
			}
		});
		return;
	}
	auto next = std::move(_usernameQueue.front());
	_usernameQueue.pop_front();
	const auto now = base::unixtime::now();
	if (Storage::Get(_session, kBlockedUntilKey).toInteger() > now) {
		next.done(false);
		pumpUsernames();
		return;
	}
	auto window = Storage::Get(_session, kWindowKey).toInteger();
	auto spent = Storage::Get(_session, kSpentKey).toInt();
	if (now - window >= kUsernameWindow) {
		window = now;
		spent = 0;
	}
	if (spent >= kUsernameBudget) {
		next.done(false);
		pumpUsernames();
		return;
	}
	Storage::Set(_session, kWindowKey, qint64(window));
	Storage::Set(_session, kSpentKey, spent + 1);

	_usernameInFlight = true;
	_usernameLastAt = crl::now();
	const auto item = next.item;
	const auto done = next.done;
	_session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(item->username),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		if (!weak) {
			return;
		}
		_usernameInFlight = false;
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		const auto channel = _session->data().channelLoaded(
			ChannelId(item->channelId));
		LOG(("Svipe Reels: %1 resolveUsername -> channel %2"
			).arg(PostUrl(*item)
			).arg(Logs::b(channel != nullptr)));
		if (channel) {
			rememberChannel(channel);
			tryMessage(item, done);
		} else {
			done(false);
		}
		pumpUsernames();
	}).fail([=](const MTP::Error &error) {
		if (!weak) {
			return;
		}
		_usernameInFlight = false;
		LOG(("Svipe Reels: %1 resolveUsername failed: %2"
			).arg(PostUrl(*item), error.type()));
		if (const auto seconds = FloodSeconds(error)) {
			Storage::Set(
				_session,
				kBlockedUntilKey,
				qint64(base::unixtime::now() + std::min(seconds, kMaxBlock)));
		}
		done(false);
		pumpUsernames();
	}).handleFloodErrors().send();
}

void Feed::requireMessage(
		const std::shared_ptr<Item> &item,
		Fn<void(HistoryItem*)> done) {
	if (item->fullId) {
		if (const auto message = _session->data().message(item->fullId)) {
			done(message);
			return;
		}
	}
	const auto weak = base::make_weak(this);
	const auto take = [=](bool ok) {
		if (!weak) {
			return;
		}
		done(ok ? _session->data().message(item->fullId) : nullptr);
	};
	tryMessage(item, [=](bool ok) {
		if (!weak) {
			return;
		} else if (ok) {
			take(true);
		} else {
			tryUsername(item, take);
		}
	});
}

void Feed::upgradeToMessage(const std::shared_ptr<Item> &item, Fn<void()> done) {
	if (item->fullId || !knownChannel(item->channelId)) {
		return;
	}
	const auto weak = base::make_weak(this);
	tryMessage(item, [=](bool ok) {
		if (weak && ok && done) {
			done();
		}
	});
}

void Feed::loadSocial(const std::shared_ptr<Item> &item) {
	if (item->socialLoaded || item->socialLoading) {
		return;
	}
	item->socialLoading = true;
	const auto weak = base::make_weak(this);
	const auto path = u"/v1/videos/state?channel_id=%1&message_id=%2"_q
		.arg(item->channelId)
		.arg(item->messageId.bare);
	Auth::EnsureToken(_session, [=](QString token) {
		if (!weak) {
			return;
		} else if (token.isEmpty()) {
			item->socialLoading = false;
			return;
		}
		Api::Get(path, token, [=](QJsonObject result, int code) {
			if (!weak) {
				return;
			}
			item->socialLoading = false;
			if (!result.contains(u"likes"_q)) {
				return;
			}
			item->socialLoaded = true;
			item->likes = result.value(u"likes"_q).toInt();
			item->liked = result.value(u"liked"_q).toBool();
			_follows[item->channelId] = Follow{
				.following = result.value(u"following"_q).toBool(),
				.followers = result.value(u"followers"_q).toInt(),
			};
			_updates.fire({});
		});
	});
}

void Feed::toggleLike(const std::shared_ptr<Item> &item) {
	item->liked = !item->liked;
	item->likes = std::max(item->likes + (item->liked ? 1 : -1), 0);
	sendEvent(*item, item->liked ? u"LIKE"_q : u"UNLIKE"_q);
	_updates.fire({});
}

void Feed::toggleFollow(const std::shared_ptr<Item> &item) {
	auto &follow = _follows[item->channelId];
	follow.following = !follow.following;
	follow.followers = std::max(follow.followers + (follow.following ? 1 : -1), 0);
	sendEvent(*item, follow.following ? u"FOLLOW"_q : u"UNFOLLOW"_q);
	_updates.fire({});
}

bool Feed::following(uint64 channelId) const {
	const auto i = _follows.find(channelId);
	return (i != end(_follows)) && i->second.following;
}

int Feed::followers(uint64 channelId) const {
	const auto i = _follows.find(channelId);
	return (i != end(_follows)) ? i->second.followers : 0;
}

void Feed::saveDocument(const std::shared_ptr<Item> &item, Fn<void(ChannelData*)> done) {
	const auto document = item->document;
	if (!document) {
		done(nullptr);
		return;
	}
	const auto link = !item->shareUrl.isEmpty()
		? item->shareUrl
		: u"https://t.me/%1/%2"_q.arg(item->username).arg(item->messageId.bare);
	const auto weak = base::make_weak(this);
	ensureSavedChannel([=](ChannelData *channel) {
		if (!weak || !channel) {
			done(nullptr);
			return;
		}
		auto message = ::Api::MessageToSend(
			::Api::SendAction(_session->data().history(channel)));
		message.textWithTags = { link };
		::Api::SendExistingDocument(std::move(message), document);
		done(channel);
	});
}

void Feed::block(uint64 channelId) {
	if (!isBlocked(channelId)) {
		auto list = Storage::Get(_session, kBlockedKey).toArray();
		list.push_back(QString::number(channelId));
		while (list.size() > 1000) {
			list.removeFirst();
		}
		Storage::Set(_session, kBlockedKey, list);
	}
	_items.erase(ranges::remove_if(_items, [&](const auto &item) {
		return (item->channelId == channelId);
	}), end(_items));
	_updates.fire({});
}

ChannelData *Feed::findSavedChannel() const {
	const auto stored = Storage::Get(_session, kSavedKey).toString().toULongLong();
	if (stored) {
		const auto channel = _session->data().channelLoaded(ChannelId(stored));
		if (channel && channel->amIn()) {
			return channel;
		}
	}
	// The phone may have made it already: the same private channel, by any of its names.
	const auto titles = SavedClipsTitles();
	auto result = (ChannelData*)nullptr;
	_session->data().enumerateBroadcasts([&](not_null<ChannelData*> channel) {
		if (!result
			&& channel->amCreator()
			&& !channel->isPublic()
			&& titles.contains(channel->name())) {
			result = channel;
		}
	});
	return result;
}

void Feed::ensureSavedChannel(Fn<void(ChannelData*)> done) {
	if (const auto channel = findSavedChannel()) {
		if (SavedClipsOldTitles().contains(channel->name())) {
			_session->api().request(MTPchannels_EditTitle(
				channel->inputChannel(),
				MTP_string(Tr(Str::ReelsSavedChannel))
			)).done([=](const MTPUpdates &result) {
				_session->api().applyUpdates(result);
			}).send();
		}
		Storage::Set(
			_session,
			kSavedKey,
			QString::number(peerToChannel(channel->id).bare));
		done(channel);
		return;
	}
	_savedWaiters.push_back(std::move(done));
	if (_savedWaiters.size() > 1) {
		return; // a creation is in flight: ride along with it
	}
	const auto weak = base::make_weak(this);
	const auto finish = [=](ChannelData *channel) {
		if (!weak) {
			return;
		}
		if (channel) {
			Storage::Set(
				_session,
				kSavedKey,
				QString::number(peerToChannel(channel->id).bare));
		}
		for (const auto &waiter : base::take(_savedWaiters)) {
			waiter(channel);
		}
	};
	using Flag = MTPchannels_CreateChannel::Flag;
	_session->api().request(MTPchannels_CreateChannel(
		MTP_flags(Flag::f_broadcast),
		MTP_string(Tr(Str::ReelsSavedChannel)),
		MTP_string(),
		MTPInputGeoPoint(),
		MTPstring(),
		MTP_int(0)
	)).done([=](const MTPUpdates &result) {
		_session->api().applyUpdates(result);
		auto channel = (ChannelData*)nullptr;
		const auto chats = [&]() -> const QVector<MTPChat>* {
			switch (result.type()) {
			case mtpc_updates: return &result.c_updates().vchats().v;
			case mtpc_updatesCombined:
				return &result.c_updatesCombined().vchats().v;
			}
			return nullptr;
		}();
		if (chats && !chats->empty() && chats->front().type() == mtpc_channel) {
			channel = _session->data().channel(
				chats->front().c_channel().vid());
		}
		if (channel) {
			// Archived, so a service channel never sits at the top of the chat list.
			_session->api().request(MTPfolders_EditPeerFolders(
				MTP_vector<MTPInputFolderPeer>(1, MTP_inputFolderPeer(
					channel->input(),
					MTP_int(1)))
			)).done([=](const MTPUpdates &updates) {
				_session->api().applyUpdates(updates);
			}).send();
		}
		finish(channel);
	}).fail([=] {
		finish(nullptr);
	}).send();
}

void Feed::save(not_null<HistoryItem*> message, Fn<void(ChannelData*)> done) {
	const auto weak = base::make_weak(this);
	const auto fullId = message->fullId();
	ensureSavedChannel([=](ChannelData *channel) {
		const auto message = weak ? _session->data().message(fullId) : nullptr;
		if (!channel || !message) {
			done(nullptr);
			return;
		}
		auto action = ::Api::SendAction(_session->data().history(channel));
		_session->api().forwardMessages(
			Data::ResolvedForwardDraft{ .items = { message } },
			action);
		done(channel);
	});
}

void Feed::sendEvent(
		const Item &item,
		const QString &type,
		QJsonObject payload) {
	auto event = QJsonObject{
		{ u"channel_id"_q, qint64(item.channelId) },
		{ u"message_id"_q, qint64(item.messageId.bare) },
		{ u"event_type"_q, type },
	};
	// The item's own page id, and only while the server still holds that page's context.
	if (!item.recId.isEmpty()
		&& (crl::now() - item.recAt) < (RecTtl() - kRecSafetyMargin)) {
		event.insert(u"recommendation_id"_q, item.recId);
	}
	if (!payload.isEmpty()) {
		event.insert(u"payload"_q, payload);
	}
	const auto body = QJsonObject{
		{ u"events"_q, QJsonArray{ event } },
	};
	Auth::EnsureToken(_session, [=](QString token) {
		if (!token.isEmpty()) {
			Api::Post(u"/v1/events"_q, body, token, nullptr);
		}
	});
}

void SelfTest(not_null<Main::Session*> session) {
	// Debug aid, silent: resolve the first clips of a page and log each route, no UI, no sound.
	const auto feed = new Feed(session);
	const auto started = std::make_shared<bool>(false);
	feed->updates() | rpl::on_next([=] {
		if (*started || feed->items().empty()) {
			return;
		}
		*started = true;
		const auto count = std::min(int(feed->items().size()), 4);
		for (auto i = 0; i != count; ++i) {
			const auto item = feed->items()[i];
			feed->resolve(item, true, [=] {
				LOG(("Svipe Reels SelfTest: %1/%2 -> document %3, failed %4"
					).arg(item->username
					).arg(item->messageId.bare
					).arg(Logs::b(item->document != nullptr)
					).arg(Logs::b(item->failed)));
			});
		}
	}, session->lifetime());
	feed->loadMore();
}

} // namespace Svipe::Reels
