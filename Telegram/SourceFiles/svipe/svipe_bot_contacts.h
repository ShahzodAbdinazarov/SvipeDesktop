/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A mini app's access to the user's Telegram contacts, ported from the Android app
(SvipeBotContacts.java), which models it on Telegram's own bot geolocation permission: the bot asks
from its mini app, the user answers once in a dialog naming the bot, the answer is kept per account
and per bot, and the bot's profile carries a switch that changes it at any time.

What is shared is the Telegram contact list the app already holds (names, username, and the phone
number only when Telegram shows it to the user). Nothing is shared before the user allows it; a
declined or revoked bot gets {status: "cancelled"}, never data.

Mini app side (sent like any Telegram.WebView event):
  web_app_request_contacts        -> contacts_requested {status, contacts?}
  web_app_check_contacts          -> contacts_checked   {available, access_requested, access_granted?}
  web_app_open_contacts_settings  -> the bot's profile, where the switch is
*/
#pragma once

#include <QtCore/QJsonObject>

class UserData;

namespace Svipe::BotContacts {

[[nodiscard]] bool Asked(not_null<UserData*> bot);
[[nodiscard]] bool Granted(not_null<UserData*> bot);

// The profile switch: the user flips it, so it takes effect without another dialog.
void SetGranted(not_null<UserData*> bot, bool granted);

[[nodiscard]] QJsonObject CheckObject(not_null<UserData*> bot);
[[nodiscard]] QJsonObject ContactsObject(not_null<UserData*> bot);

// Fires with the bot's id when an answer changes, so open mini apps and profiles follow it.
[[nodiscard]] rpl::producer<uint64> Changes();

} // namespace Svipe::BotContacts
