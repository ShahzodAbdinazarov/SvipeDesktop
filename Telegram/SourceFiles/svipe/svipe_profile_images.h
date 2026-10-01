/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The profile's "Profile Images" tab (Android: SvipeProfileImages + SvipeProfileImagesAdapter, the last
tab of the profile media pager): a person's current profile photos and the ones we kept after they
deleted them, newest set first, the deleted ones with a red "Deleted".

Built the way the Stories tab is: a sub-controller selects a Media::ListProvider, the provider wraps
each photo into a fake history item, and the media grid draws it with its own Overview photo cell —
so spacing, hover and the viewer are tdesktop's, not a copy.
*/
#pragma once

#include "info/profile/tabs/info_profile_tab_content.h"
#include "ui/style/style_core_icon.h"

class UserData;

namespace Ui {
class VerticalLayout;
class MultiSlideTracker;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Info {
class AbstractController;
} // namespace Info

namespace Info::Media {
class ListProvider;
} // namespace Info::Media

namespace Svipe::ProfileImages {

[[nodiscard]] Info::Profile::MediaTabDescriptor MakeTabDescriptor(
	not_null<UserData*> user);

// The same grid for the classic profile (shared media as a list of counted buttons — the default;
// the tabs are an upstream work-in-progress option): a "N profile images" button opening it in a box.
void AddClassicButton(
	not_null<Ui::VerticalLayout*> parent,
	not_null<Window::SessionController*> window,
	not_null<UserData*> user,
	Ui::MultiSlideTracker &tracker,
	const style::icon &icon);

// The grid provider when the list belongs to this tab, nullptr otherwise (Info::Media::MakeProvider).
[[nodiscard]] std::unique_ptr<Info::Media::ListProvider> MakeProvider(
	not_null<Info::AbstractController*> controller);

} // namespace Svipe::ProfileImages
