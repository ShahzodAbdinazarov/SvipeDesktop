/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The Bots notification category (Android: SvipeBotNotificationsActivity): one switch for new messages
from bots, the bots' message-type rules, and the bots that keep notifying while the rest are muted.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Svipe {

void BotsNotificationsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

} // namespace Svipe
