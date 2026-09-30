/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The Bots notification category as a settings page of its own, laid out like Private Chats next to it
(Android: SvipeBotNotificationsActivity, made "the category screen it sits next to" in 30b935915).
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Settings {

class SvipeNotificationsBots : public Section<SvipeNotificationsBots> {
public:
	SvipeNotificationsBots(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

};

} // namespace Settings
