/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A Svipe backend token for the account (the Android app's SvipeAuth.java): the stored access token
while it is fresh, then a refresh, then the Mini App initData from the auth bot's menu button, which
the backend exchanges for tokens. Concurrent callers share one flow, and every step has a deadline,
so a caller is always answered — with an empty string when there is no token to be had.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Svipe::Auth {

void EnsureToken(not_null<Main::Session*> session, Fn<void(QString)> done);
[[nodiscard]] QString ExtractInitData(const QString &webViewUrl);

} // namespace Svipe::Auth
