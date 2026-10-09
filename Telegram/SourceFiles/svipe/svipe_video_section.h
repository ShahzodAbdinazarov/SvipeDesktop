/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The Video tab (Android: SvipeSearchActivity + SvipeExploreGrid + SvipeWatchActivity), laid out the
way YouTube is on desktop: a grid of wide cards — the poster, the duration, the channel, two lines of
title, "channel · views · date" — and a watch page with the player, the title, the channel with
Subscribe, like / share / save, the caption, and related videos beside it (below it when the window is
narrow). Like Clips it covers everything right of the folders sidebar.
*/
#pragma once

#include "ui/rp_widget.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Svipe::Video {

void Open(not_null<Window::SessionController*> controller);
void Close(not_null<Window::SessionController*> controller);
[[nodiscard]] bool Shown(not_null<Window::SessionController*> controller);
[[nodiscard]] rpl::producer<bool> ShownValue(
	not_null<Window::SessionController*> controller);

// Android's SvipeVideoWarmer: have the first screen ready before the tab is opened — the first page
// fetched and its first cards resolved — for someone who has opened Video before.
void Warm(not_null<Window::SessionController*> controller);

// Debug aid: open the first video of the grid once the list has one.
void DebugOpenFirst(not_null<Window::SessionController*> controller);

} // namespace Svipe::Video
