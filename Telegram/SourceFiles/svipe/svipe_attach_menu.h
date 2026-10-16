/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Any bot with a mini app in the attachment menu, with the user's consent; ported from the Android app
(SvipeAttachMenu.java). Telegram's server refuses toggleBotInAttachMenu for every bot it didn't pick,
so the list is kept on the client, per account, and the menu shows it after Telegram's own bots.

Ways in, as on Android:
  t.me/<bot>?startattach for a bot Telegram didn't approve (Telegram says "not supported");
  the mini app itself:
    web_app_add_to_attach_menu -> attach_menu_added, or attach_menu_failed {error}
    web_app_check_attach_menu  -> attach_menu_checked {status: added | missed | declined}
A mini app gets the dialog once: after a decline or a removal it gets USER_DECLINED without asking.

Opening: a bot with a Main Mini App opens it in the current chat (requestMainWebView); a bot with only
a menu button opens it as from its own chat, because the server refuses a menu button URL anywhere
else (BOT_WEBVIEW_DISABLED).
*/
#pragma once

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class Show;
} // namespace Ui

namespace Svipe::AttachMenu {

[[nodiscard]] std::vector<not_null<UserData*>> Bots(
	not_null<Main::Session*> session);
[[nodiscard]] bool Contains(not_null<UserData*> bot);

// "added", "declined" (declined or removed: a mini app is not asked again) or "missed".
[[nodiscard]] QString Status(not_null<UserData*> bot);

// Only a bot with a mini app can be opened from the menu.
[[nodiscard]] bool CanAdd(not_null<UserData*> bot);

// Telegram's "add to attachment menu" dialog for this bot. done(shownNow, added).
// fromMiniApp: a declined bot is not asked again.
void Request(
	std::shared_ptr<Ui::Show> show,
	not_null<UserData*> bot,
	bool fromMiniApp,
	Fn<void(bool shownNow, bool added)> done);

// Right click on the button. A mini app is not asked again after this.
void Remove(std::shared_ptr<Ui::Show> show, not_null<UserData*> bot);

// Fires when the list changes, so open menus are rebuilt.
[[nodiscard]] rpl::producer<> Changes();

} // namespace Svipe::AttachMenu
