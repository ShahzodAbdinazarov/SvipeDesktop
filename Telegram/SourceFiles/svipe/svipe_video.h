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

class List final : public base::has_weak_ptr {
public:
	// `path` is the endpoint with any fixed query already on it, e.g. "/v1/videos" or
	// "/v1/videos/related?seed_channel_id=1&seed_message_id=2".
	List(not_null<Reels::Feed*> feed, QString path, int pageSize);

	[[nodiscard]] const std::vector<std::shared_ptr<Item>> &items() const {
		return _items;
	}
	[[nodiscard]] bool exhausted() const {
		return _exhausted;
	}
	[[nodiscard]] bool loading() const {
		return _loading;
	}
	[[nodiscard]] QString status() const {
		return _status;
	}
	[[nodiscard]] rpl::producer<> updates() const {
		return _updates.events();
	}

	void loadMore();
	// Start over from the first page in place: whoever holds this list keeps a valid one.
	void reset();
	// Drop one reference (a seed that came back in its own related list, a dead post).
	void remove(const std::shared_ptr<Item> &item);

private:
	const not_null<Reels::Feed*> _feed;
	const QString _path;
	const int _pageSize = 20;
	std::vector<std::shared_ptr<Item>> _items;
	int _offset = 0;
	int _failures = 0;
	int _generation = 0; // a reset orphans whatever request was in flight
	bool _loading = false;
	bool _exhausted = false;
	QString _status;
	rpl::event_stream<> _updates;

};

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
