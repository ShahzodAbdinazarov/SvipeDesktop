/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_config.h"

namespace Svipe {

QString BaseUrl() {
#ifdef _DEBUG
	return u"https://dev.svipe.uz"_q;
#else // _DEBUG
	return u"https://svipe.uz"_q;
#endif // _DEBUG
}

QString AuthBotUsername() {
#ifdef _DEBUG
	return u"Lavha_auth_bot"_q;
#else // _DEBUG
	return u"Svipe_auth_bot"_q;
#endif // _DEBUG
}

QString WebAppUrl() {
	return BaseUrl() + u"/webapp"_q;
}

} // namespace Svipe
