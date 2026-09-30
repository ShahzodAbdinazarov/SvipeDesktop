/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The screens that write the message-type rules (Android: SvipeMessageTypesActivity for a class of
chats, the "Message types" section of ProfileNotificationsActivity for one chat).
*/
#pragma once

#include "svipe/svipe_strings.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Svipe {

// Both lists for a class of chats: "all", "private", "groups", "channels" or "bots".
void MessageTypesScopeBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session,
	QString scope,
	Str title);

// One chat: the list that applies to it — muted kinds while the chat rings, notified kinds while
// it is muted — exactly as the Android chat settings show it.
void MessageTypesChatBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer);

} // namespace Svipe
