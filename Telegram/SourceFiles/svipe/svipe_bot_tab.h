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
class UserData;

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

// Bots' tabs on a person's own profile (Android: SvipeProfileTabs; svipe.uz/bots/profile-tabs#profiles).
// A registered bot asks with svipe.uz/<bot>?profiletab or addToProfile(); the person agrees here and
// the tab is live at once: at most three, a fourth only in place of one the person picks.

// The index-th tab this person's profile shows the viewer (0..2), for the tabbed profile.
[[nodiscard]] Info::Profile::MediaTabDescriptor MakeProfileTabDescriptor(
	not_null<UserData*> user,
	int index);

// The same for the classic profile: up to three buttons named by their bots.
void AddProfileClassicButtons(
	not_null<Ui::VerticalLayout*> parent,
	not_null<Window::SessionController*> window,
	not_null<UserData*> user,
	Ui::MultiSlideTracker &tracker,
	const style::icon &icon);

// The bot asks; done gets (whether a box was shown, whether the tab is on my profile now). From a
// mini app a bot told "no" this run isn't asked again.
void RequestProfileTab(
	std::shared_ptr<Ui::Show> show,
	not_null<UserData*> bot,
	bool fromMiniApp,
	Fn<void(bool shownNow, bool added)> done);

// t.me|svipe.uz/<bot>?profiletab
void RequestProfileTabByUsername(
	not_null<Window::SessionController*> controller,
	const QString &username);

// For checkProfileTabStatus(): "added", "missed" or "declined".
void ProfileTabStatus(not_null<UserData*> bot, Fn<void(QString)> done);

// The profile whose tab card just opened this bot's mini app (within half a minute), taken once.
[[nodiscard]] UserData *TakeOpenedFromProfile(not_null<UserData*> bot);

} // namespace Svipe::BotTab
