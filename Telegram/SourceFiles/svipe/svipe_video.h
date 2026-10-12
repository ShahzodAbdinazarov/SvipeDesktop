/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The Video tab's lists (Android: SvipeDiscover's long-form pipe, SvipeExploreGrid, the watch page's
related list) and its player telemetry (SvipeVideoTelemetry + SvipeLongWatch).

A list is a paged GET over offsets: ``/v1/videos`` for the tab, ``/v1/videos/related`` seeded with a
post for the watch page. Items are the same references the Clips feed holds, so they are resolved,
saved and reported through Reels::Feed — one resolver, one flood discipline, for both surfaces.
*/
#pragma once

#include "base/weak_ptr.h"
#include "svipe/svipe_reels.h"

namespace Svipe::Video {

using Reels::Item;

// What a grid shows: the tab's list, a search's two pipes, or the recent searches.
class Source {
public:
	virtual ~Source() = default;

	[[nodiscard]] virtual const std::vector<std::shared_ptr<Item>> &items() const = 0;
	[[nodiscard]] virtual bool exhausted() const = 0;
	[[nodiscard]] virtual QString status() const = 0;
	[[nodiscard]] virtual rpl::producer<> updates() const = 0;
	virtual void loadMore() = 0;
	virtual void remove(const std::shared_ptr<Item> &item) = 0;
	// How many wide cards / portrait tiles make a row, so a mixed source can order whole rows.
	virtual void setRowSizes(int wide, int tall) {
	}

};

class List final : public Source, public base::has_weak_ptr {
public:
	// `path` is the endpoint with any fixed query already on it, e.g. "/v1/videos" or
	// "/v1/videos/related?seed_channel_id=1&seed_message_id=2".
	List(not_null<Reels::Feed*> feed, QString path, int pageSize);

	[[nodiscard]] const std::vector<std::shared_ptr<Item>> &items() const override {
		return _items;
	}
	[[nodiscard]] bool exhausted() const override {
		return _exhausted;
	}
	[[nodiscard]] bool loading() const {
		return _loading;
	}
	[[nodiscard]] bool failedOut() const {
		return _failures >= 2 && _items.empty();
	}
	[[nodiscard]] bool answered() const {
		return _answered;
	}
	[[nodiscard]] QString status() const override {
		return _status;
	}
	[[nodiscard]] rpl::producer<> updates() const override {
		return _updates.events();
	}

	void loadMore() override;
	// Start over from the first page in place: whoever holds this list keeps a valid one.
	void reset();
	// Drop one reference (a seed that came back in its own related list, a dead post).
	void remove(const std::shared_ptr<Item> &item) override;
	// Items from this list are reels (the shorts pipe), drawn as portrait tiles.
	void markShortForm() {
		_shortForm = true;
	}

private:
	const not_null<Reels::Feed*> _feed;
	const QString _path;
	const int _pageSize = 20;
	std::vector<std::shared_ptr<Item>> _items;
	int _offset = 0;
	int _failures = 0;
	int _generation = 0; // a reset orphans whatever request was in flight
	bool _loading = false;
	bool _answered = false;
	bool _shortForm = false;
	bool _exhausted = false;
	QString _status;
	rpl::event_stream<> _updates;

};

// Android SvipeExploreGrid's search: both pipes at once — reels from /v1/discover/search (60 a
// page) and long video from /v1/videos/search (20 a page) — shown as rows of wide cards and rows of
// portrait tiles, alternating. The first screen waits for both; a long pipe that fails out leaves
// the results to reels alone.
class SearchSource final : public Source {
public:
	SearchSource(not_null<Reels::Feed*> feed, const QString &query);

	[[nodiscard]] const std::vector<std::shared_ptr<Item>> &items() const override {
		return _merged;
	}
	[[nodiscard]] bool exhausted() const override;
	[[nodiscard]] QString status() const override;
	[[nodiscard]] rpl::producer<> updates() const override {
		return _updates.events();
	}
	void loadMore() override;
	void remove(const std::shared_ptr<Item> &item) override;
	void setRowSizes(int wide, int tall) override;

	[[nodiscard]] const QString &query() const {
		return _query;
	}

private:
	void merge();

	const QString _query;
	List _shorts;
	List _longs;
	std::vector<std::shared_ptr<Item>> _merged;
	int _wide = 3;
	int _tall = 5;
	rpl::event_stream<> _updates;
	rpl::lifetime _lifetime;

};

// The recent searches: the videos tapped in search results (Android: SvipeVideoSearchHistory, kept on
// the device only — 20, newest first). Tapping one reopens that video; it does not search again.
class RecentSource final : public Source {
public:
	explicit RecentSource(not_null<Main::Session*> session);

	[[nodiscard]] const std::vector<std::shared_ptr<Item>> &items() const override {
		return _items;
	}
	[[nodiscard]] bool exhausted() const override {
		return true;
	}
	[[nodiscard]] QString status() const override {
		return QString();
	}
	[[nodiscard]] rpl::producer<> updates() const override {
		return _updates.events();
	}
	void loadMore() override {
	}
	void remove(const std::shared_ptr<Item> &item) override;
	void clear();

private:
	const not_null<Main::Session*> _session;
	std::vector<std::shared_ptr<Item>> _items;
	rpl::event_stream<> _updates;

};

namespace SearchHistory {

[[nodiscard]] bool Empty(not_null<Main::Session*> session);
void Add(not_null<Main::Session*> session, const Item &item);

} // namespace SearchHistory

// SvipeVideoTelemetry: one watch of one long video. IMPRESSION on open, PLAY_START, FIRST_FRAME,
// a HEARTBEAT every 30 s while playing (and on pause), and one terminal event on leave, classified
// the way SvipeLongWatch does.
class Watch final {
public:
	Watch(not_null<Reels::Feed*> feed, std::shared_ptr<Item> item);
	~Watch();

	void playing(bool playing, crl::time position, crl::time duration);
	void firstFrame();
	void buffering(bool buffering);
	void ended();

private:
	[[nodiscard]] QJsonObject payload() const;
	void send(const QString &type, QJsonObject extra = QJsonObject());
	void heartbeat();

	const not_null<Reels::Feed*> _feed;
	const std::shared_ptr<Item> _item;
	const crl::time _openedAt = 0;
	crl::time _playStartedAt = 0;
	crl::time _watched = 0;
	crl::time _bufferingSince = 0;
	crl::time _buffering = 0;
	crl::time _firstFrameAfter = -1;
	crl::time _position = 0;
	crl::time _duration = 0;
	crl::time _lastHeartbeat = 0;
	bool _playStartSent = false;
	bool _ended = false;
	rpl::lifetime _heartbeatLifetime;

};

} // namespace Svipe::Video
