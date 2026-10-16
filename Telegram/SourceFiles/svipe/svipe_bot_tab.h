/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A bot's tab on a channel profile (Android: SvipeBotTab, SvipeBotTabAdapter, SvipeBotTabSettingsActivity;
svipe.uz/bots/profile-tabs). A channel admin asks a bot registered with Svipe for its tab; the bot checks
that person is an admin and accepts; the bot names the tab and fills it with cards. A card opens the
bot's mini app. Card images come from the Svipe server, never from the bot's.
*/
#pragma once

#include "info/profile/tabs/info_profile_tab_content.h"
#include "ui/style/style_core_icon.h"

class ChannelData;

namespace Ui {
class VerticalLayout;
class MultiSlideTracker;
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Svipe::BotTab {

// The tab, for the tabbed profile.
[[nodiscard]] Info::Profile::MediaTabDescriptor MakeTabDescriptor(
	not_null<ChannelData*> channel);

// The same cards for the classic profile: a button named by the bot, opening them in a box.
void AddClassicButton(
	not_null<Ui::VerticalLayout*> parent,
	not_null<Window::SessionController*> window,
	not_null<ChannelData*> channel,
	Ui::MultiSlideTracker &tracker,
	const style::icon &icon);

// Channel settings → Bot tab.
void ShowSettingsBox(
	std::shared_ptr<Ui::Show> show,
	not_null<ChannelData*> channel);

} // namespace Svipe::BotTab
