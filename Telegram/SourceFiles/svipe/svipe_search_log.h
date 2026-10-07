/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Search history for the native chats search (Android: SvipeSearchLog with source "chats", driven from
DialogsActivity.svipeLogNativeQuery / svipeLogNativeClick). One visit = one session id; the settled
queries typed during it (progressive typing collapsed) and the result tapped go to
POST /v1/search/session, which keeps the visit as one record.

Privacy, as on Android: the QUERY goes up, and of a tapped result only a doorway — a public broadcast
channel by handle and title (catalogue data), anything else by its handle or bare id with no name.
A tapped message never sends its text, only the chat it lives in. Searching inside one chat is not
recorded at all.
*/
#pragma once

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::SearchLog {

// Debounced; an empty query ends the visit.
void Query(not_null<Main::Session*> session, const QString &text);
void Click(not_null<PeerData*> peer);

} // namespace Svipe::SearchLog
