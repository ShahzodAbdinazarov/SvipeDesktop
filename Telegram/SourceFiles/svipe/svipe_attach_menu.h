/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Any bot with a mini app in the attachment menu, with the user's consent; ported from the Android app
(SvipeAttachMenu.java). Telegram's server refuses toggleBotInAttachMenu for every bot it didn't pick,
so the list is kept on the client, per account, and the menu shows it after Telegram's own bots.

Ways in, as on Android:
  t.me/<bot>?startattach, t.me/<chat>?attach=<bot> and ?startattach&choose=… for a bot Telegram
  didn't approve (Telegram says "not supported");
  the mini app itself:
    web_app_add_to_attach_menu -> attach_menu_added, or attach_menu_failed {error}
    web_app_check_attach_menu  -> attach_menu_checked {status: added | missed | declined}
A mini app gets the dialog once: after a decline or a removal it gets USER_DECLINED without asking.

Opening: a bot with a Main Mini App opens it in the current chat (requestMainWebView); a bot with only
a menu button opens it as from its own chat, because the server refuses a menu button URL anywhere
else (BOT_WEBVIEW_DISABLED).

What Telegram keeps on its server for the bots it picked comes from Svipe's (app/api/attach.py):
  the bot's own settings (GET /v1/attach/config): the chats the button shows in, the side menu, and
  whether the add dialog asks to let the bot write to the user;
  the same list on every device of the account (/v1/attach/bots);
  who the chat is with, signed by Svipe and passed to the mini app as tgWebAppSvipeData (SignContext);
  answerWebAppQuery: the mini app hands over a message its bot prepared, and the app sends it to the
  chat the menu was opened in (web_app_send_to_chat, SendPrepared).
*/
#pragma once

#include "inline_bots/bot_attach_web_view.h"

class UserData;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class Show;
} // namespace Ui

namespace Svipe::AttachMenu {

// The bots to show, in the order they were added. The bots' settings and the other devices'
// answers are fetched once per run, on the first call.
[[nodiscard]] std::vector<not_null<UserData*>> Bots(
	not_null<Main::Session*> session);
// Those of them that asked for the side menu too.
[[nodiscard]] std::vector<not_null<UserData*>> SideMenuBots(
	not_null<Main::Session*> session);
[[nodiscard]] bool Contains(not_null<UserData*> bot);

// The chats the bot's button shows in, as the bot set them; every chat but its own by default.
[[nodiscard]] InlineBots::PeerTypes Types(not_null<UserData*> bot);

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

// Opens the bot's mini app from the menu in this chat or, with no chat, from the side menu: a bot
// with a Main Mini App opens it there, one with only a menu button as from its own chat.
void Open(
	std::shared_ptr<Ui::Show> show,
	base::weak_ptr<Window::SessionController> controller,
	not_null<UserData*> bot,
	std::optional<::Api::SendAction> chat,
	const QString &startCommand = QString());

// Receiver / chat for the mini app of a bot opened from this menu in `peer`, signed by Svipe.
// done gets the URL to load, with the launch param tgWebAppSvipeData added, or the URL as it was if
// the signature doesn't come in time. Always called once, on the main thread.
void SignContext(
	not_null<UserData*> bot,
	not_null<PeerData*> peer,
	const QString &url,
	Fn<void(QString url)> done);

// Sends a message the bot prepared (Bot API savePreparedInlineMessage) with `action`, as
// answerWebAppQuery sends one for Telegram's own attach bots. done gets an empty string when sent,
// or the error a mini app is told: MESSAGE_EXPIRED, or MESSAGE_SEND_FAILED (also when the bot didn't
// allow this kind of chat for the message).
void SendPrepared(
	not_null<UserData*> bot,
	const QString &id,
	::Api::SendAction action,
	Fn<void(QString error)> done);

} // namespace Svipe::AttachMenu
