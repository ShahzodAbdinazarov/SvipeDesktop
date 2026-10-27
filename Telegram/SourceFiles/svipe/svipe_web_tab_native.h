/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The platform half of a bot's page inside a profile tab (svipe_web_tab.h): a native view that clips the
web view it holds, and the scroll wheel taken from the web view while the profile still has to scroll.
*/
#pragma once

#include <QtCore/QPoint>

class QWidget;

namespace Svipe::WebTabNative {

// Whether TakeWheel works here (macOS). Elsewhere the page itself hands the wheel over (a script in
// the page, svipe_web_tab.cpp), as the web view runs in another process the app can't reach into.
[[nodiscard]] bool TakesWheel();

// `widget` is native: what it holds is cut to its bounds (a child window already is, on Windows).
void ClipToBounds(not_null<QWidget*> widget);

struct Wheel {
	QPoint pixelDelta;
	QPoint angleDelta;
	bool inverted = false;
	Qt::ScrollPhase phase = Qt::NoScrollPhase;
	Qt::KeyboardModifiers modifiers;
};

// While the lifetime lives, a scroll over `widget` goes to `take` instead of the web view under it
// whenever `wanted()` says so.
[[nodiscard]] rpl::lifetime TakeWheel(
	not_null<QWidget*> widget,
	Fn<bool()> wanted,
	Fn<void(Wheel)> take);

} // namespace Svipe::WebTabNative
