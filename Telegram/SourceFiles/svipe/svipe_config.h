/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Where this build talks to: the dev server and dev auth bot for Debug builds, production otherwise.
The same split as the Android app's .beta vs .web/Play (SvipeConfig.java).
*/
#pragma once

#include <QtCore/QString>

namespace Svipe {

[[nodiscard]] QString BaseUrl();
[[nodiscard]] QString AuthBotUsername();
[[nodiscard]] QString WebAppUrl();

} // namespace Svipe
