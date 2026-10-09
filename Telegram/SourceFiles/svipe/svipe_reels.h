/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The Clips (reels) feed, ported from the Android app (ReelsActivity + SvipeRefResolver +
SvipeChannelResolve + SvipeRecAttribution + SvipeWatchedSet + SvipeWatchEvent).

The backend sends references only — channel id, message id, username — never a Telegram file. This
side turns a reference into a playable document, cheapest first:
  1. the channel is known with its access hash (Telegram's cache, or the hashes we keep on disk
     across launches): channels.getMessages;
  2. messages.getWebPage("https://t.me/<user>/<id>"): the post's video without resolving anyone;
  3. contacts.resolveUsername — one at a time, spaced, an hourly budget, and a FLOOD_WAIT remembered
     across launches — and only for the clip on screen.
Watch telemetry goes to POST /v1/events the way Android sends it.
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_file_origin.h"

class DocumentData;
class ChannelData;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::Reels {

struct Item {
	uint64 channelId = 0;
	MsgId messageId = 0;
	QString username;
	QString shareUrl;
	int width = 0;
	int height = 0;
	crl::time durationMs = 0;
	QString recId;
	crl::time recAt = 0;
	int feedPosition = 0;
	QByteArray inlineThumb; // Telegram's stripped blur from the list JSON (thumb_b64), if any

	// Resolved.
	DocumentData *document = nullptr;
	Data::FileOrigin origin;
	FullMsgId fullId; // set when the real channel message is known
	QString title; // the channel's name
	TextWithEntities caption;
	bool resolving = false;
	bool failed = false;
	std::vector<Fn<void()>> waiters;

	// Svipe's own like on this post (GET /v1/videos/state), not a Telegram reaction.
	int likes = 0;
	bool liked = false;
	bool socialLoaded = false;
	bool socialLoading = false;
};

class Feed final : public base::has_weak_ptr {
public:
	explicit Feed(not_null<Main::Session*> session);
	~Feed();

	[[nodiscard]] not_null<Main::Session*> session() const {
		return _session;
	}
	[[nodiscard]] const std::vector<std::shared_ptr<Item>> &items() const {
		return _items;
	}
	[[nodiscard]] bool exhausted() const {
		return _exhausted;
	}
	// Empty while fine; a line to show while the feed has nothing to play.
	[[nodiscard]] QString status() const {
		return _status;
	}
	[[nodiscard]] rpl::producer<> updates() const {
		return _updates.events();
	}

	void loadMore();

	// Playback never spends a resolveUsername: a known channel, else the post's link preview
	// (getWebPage), else the item fails and leaves its list. `urgent` / `background` only say
	// whether a failed preview is final (on screen, a list card) or worth another try later.
	void resolve(
		const std::shared_ptr<Item> &item,
		bool urgent,
		Fn<void()> done,
		bool background = false);

	// The real channel message, for anything that acts on it (like, comments, subscribe, save,
	// report). Android's requireMessage: may spend a resolve, since the user asked for it.
	void requireMessage(
		const std::shared_ptr<Item> &item,
		Fn<void(HistoryItem*)> done);

	// The real channel message for an item that has its video from a link preview, but only when the
	// channel is already addressable — never spends a resolve. For list cards: caption, views, date.
	void upgradeToMessage(const std::shared_ptr<Item> &item, Fn<void()> done);

	// Svipe's own likes and subscriptions, kept on our server: none of them needs the channel's
	// access_hash, so none of them spends a resolveUsername. The state is loaded once per item;
	// toggles are optimistic and travel as LIKE / UNLIKE / FOLLOW / UNFOLLOW events.
	void loadSocial(const std::shared_ptr<Item> &item);
	void toggleLike(const std::shared_ptr<Item> &item);
	void toggleFollow(const std::shared_ptr<Item> &item);
	[[nodiscard]] bool following(uint64 channelId) const;
	[[nodiscard]] int followers(uint64 channelId) const;

	// Save the video itself (the document a link preview gave) into "Saved Clips": no forward, so
	// no source message and no resolve. The post link rides along as the caption.
	void saveDocument(const std::shared_ptr<Item> &item, Fn<void(ChannelData*)> done);

	// Android's blockChannel: its clips leave the feed now and never come back.
	void block(uint64 channelId);

	// Android's SvipeSavedChannels: forward the post into a private, archived "Saved Clips"
	// channel the user owns — found again by its title on another device, created once.
	void save(not_null<HistoryItem*> message, Fn<void(ChannelData*)> done);

	void sendEvent(
		const Item &item,
		const QString &type,
		QJsonObject payload = QJsonObject());
	void markWatched(const Item &item);

	// SvipeWatchEvent.classify.
	[[nodiscard]] static QString Classify(crl::time watched, crl::time duration);

	// The resolve/event/save machinery is shared with the Video tab, whose lists hold Items too.
	[[nodiscard]] static std::shared_ptr<Item> ParseItem(const QJsonObject &o);

private:
	struct Follow {
		bool following = false;
		int followers = 0;
	};
	base::flat_map<uint64, Follow> _follows;

	struct UsernameRequest {
		std::shared_ptr<Item> item;
		Fn<void(bool)> done;
	};

	void request();
	void ensureSavedChannel(Fn<void(ChannelData*)> done);
	[[nodiscard]] ChannelData *findSavedChannel() const;
	void scheduleRetry();
	void tryMessage(const std::shared_ptr<Item> &item, Fn<void(bool)> done);
	void tryWebPage(
		const std::shared_ptr<Item> &item,
		Fn<void(bool)> done,
		bool retried = false);
	void tryUsername(
		const std::shared_ptr<Item> &item,
		Fn<void(bool)> done,
		bool back = false);
	void pumpUsernames();
	void finish(const std::shared_ptr<Item> &item, bool ok);
	bool takeMessage(const std::shared_ptr<Item> &item, HistoryItem *message);
	void rememberChannel(not_null<ChannelData*> channel);
	[[nodiscard]] ChannelData *knownChannel(uint64 channelId);
	[[nodiscard]] bool isWatched(uint64 channelId, MsgId messageId) const;
	[[nodiscard]] bool isBlocked(uint64 channelId) const;
	[[nodiscard]] bool contains(uint64 channelId, MsgId messageId) const;

	const not_null<Main::Session*> _session;
	std::vector<std::shared_ptr<Item>> _items;
	QString _cursor;
	bool _loading = false;
	bool _exhausted = false;
	int _emptyPages = 0;
	int _failures = 0;
	QString _status;
	rpl::event_stream<> _updates;

	std::vector<Fn<void(ChannelData*)>> _savedWaiters;

	std::deque<UsernameRequest> _usernameQueue;
	bool _usernameInFlight = false;
	crl::time _usernameLastAt = 0;

	rpl::lifetime _lifetime;

};

void SelfTest(not_null<Main::Session*> session);

} // namespace Svipe::Reels
