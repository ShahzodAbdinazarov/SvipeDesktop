/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A bot's tab on a chat's profile (Android: SvipeBotTab, SvipeBotTabAdapter, SvipeBotTabSettingsActivity;
svipe.uz/bots/profile-tabs#places). A channel's or group's admin asks a bot registered with Svipe for its
tab; the bot checks that person is an admin and accepts; the bot names the tab and fills it with cards.
Saved Messages' owner puts one there for themselves. A card opens the bot's mini app. Card images come
from the Svipe server, never from the bot's. A bot with live cards may answer a viewer with nothing
(the tab is hidden for them) or with its empty state.
*/
#pragma once

#include "info/profile/tabs/info_profile_tab_content.h"
#include "ui/style/style_core_icon.h"

class PeerData;
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

// The tab of a channel, supergroup or basic group, for the tabbed profile.
[[nodiscard]] Info::Profile::MediaTabDescriptor MakeTabDescriptor(
	not_null<PeerData*> peer);

// The same cards for the classic profile: a button named by the bot, opening them in a box.
void AddClassicButton(
	not_null<Ui::VerticalLayout*> parent,
	not_null<Window::SessionController*> window,
	not_null<PeerData*> peer,
	Ui::MultiSlideTracker &tracker,
	const style::icon &icon);

// Chat settings → Bot tab: a channel's or group's (its admins), or Saved Messages' (`peer` is me).
void ShowSettingsBox(
	std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer);

// Bots' tabs on a person's profile (Android: SvipeProfileTabs; svipe.uz/bots/profile-tabs#profiles).
// A registered bot asks with svipe.uz/<bot>?profiletab (or ?privatetab) or addToProfile(); the person
// agrees here and the tab is live at once: at most three of each kind, a fourth only in place of one.
// Kind "profile": on their profile, for everyone or only them. Kind "private": on their profile for
// everyone, and on the profile of each of the bot's other users for them, showing what concerns the two.

// Which profile screen: a person's, Saved Messages' (mine), or a bot's (its own tab).
enum class ProfileView {
	Profile,
	SavedMessages,
	Bot,
};

// The index-th tab this profile shows the viewer (0..2), for the tabbed profile.
[[nodiscard]] Info::Profile::MediaTabDescriptor MakeProfileTabDescriptor(
	not_null<UserData*> user,
	int index,
	ProfileView view = ProfileView::Profile);

// The same for the classic profile: up to three buttons named by their bots.
void AddProfileClassicButtons(
	not_null<Ui::VerticalLayout*> parent,
	not_null<Window::SessionController*> window,
	not_null<UserData*> user,
	Ui::MultiSlideTracker &tracker,
	const style::icon &icon,
	ProfileView view = ProfileView::Profile);

// The bot asks for a tab of `kind` ("profile" or "private"); done gets (whether a box was shown,
// whether the tab is mine now). From a mini app a bot told "no" this run isn't asked again.
void RequestProfileTab(
	std::shared_ptr<Ui::Show> show,
	not_null<UserData*> bot,
	bool fromMiniApp,
	const QString &kind,
	Fn<void(bool shownNow, bool added)> done);

// t.me|svipe.uz/<bot>?profiletab (kind "profile") or ?privatetab (kind "private").
void RequestProfileTabByUsername(
	not_null<Window::SessionController*> controller,
	const QString &username,
	const QString &kind);

// For checkProfileTabStatus(): "added", "missed" or "declined", for a tab of `kind`.
void ProfileTabStatus(
	not_null<UserData*> bot,
	const QString &kind,
	Fn<void(QString)> done);

// Where the bot tab card that just opened this bot's mini app was (within half a minute), taken once:
// the profile or chat, and the chat_type its mini app is signed in with.
struct OpenedFrom {
	PeerData *peer = nullptr;
	QString chatType;
};
[[nodiscard]] OpenedFrom TakeOpenedFrom(not_null<UserData*> bot);

} // namespace Svipe::BotTab
