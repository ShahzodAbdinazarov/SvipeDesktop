/*
Svipe Desktop — Svipe additions to Telegram Desktop.

"Send via Svipe" (Android: SvipeDirect.java). 1:1 text messages that travel through the Svipe
backend (/v1/direct) only, never through Telegram. Phase 1: private chats, text, polling.

On this client a Svipe message is a client-side item (MessageFlag::Local), which tdesktop already
keeps out of every server call and places by date into the loaded range of the history. The server
holds the history, so it is fetched again from the start on every launch; only the unsent outbox is
kept on disk. The wire body is the app-neutral JSON both clients share:
  {"v":1,"text":..,"entities":[{"t":"bold","o":0,"l":4},..],"reply_to":<client_id>,"fwd":{..}}
*/
#pragma once

class History;
class HistoryItem;
class PeerData;
class QPainter;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::Direct {

void Start(not_null<Main::Session*> session);
void OnChatOpened(not_null<History*> history);

// Private chats with a real person: no bots, not Saved Messages, no service accounts.
[[nodiscard]] bool CanSendTo(not_null<PeerData*> peer);
[[nodiscard]] bool IsDirect(not_null<const HistoryItem*> item);

// The reply target only travels when it is itself a Svipe message.
void Send(
	not_null<History*> history,
	TextWithEntities text,
	HistoryItem *replyTo);

// Forward a Svipe message to `to` via Svipe as well. A target Svipe cannot reach yet gets nothing
// and the user is told — the text never goes to Telegram.
void Forward(
	not_null<History*> to,
	not_null<HistoryItem*> item,
	bool dropAuthor);

// The user deleted it: my own is withdrawn for both sides on the server, the peer's only here.
void OnDelete(not_null<HistoryItem*> item);

// The mark before the time: the Svipe logo in the date colour.
[[nodiscard]] int MarkWidth();
void PaintMark(QPainter &p, int left, int top, const QColor &color);

[[nodiscard]] QString SendLabel();
[[nodiscard]] QString MenuHint();

} // namespace Svipe::Direct
