/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Windows and Linux: the web view is a child window, cut to the native host by the system, and the
page hands the wheel over itself (svipe_web_tab.cpp).
*/
#include "svipe/svipe_web_tab_native.h"

namespace Svipe::WebTabNative {

bool TakesWheel() {
	return false;
}

void ClipToBounds(not_null<QWidget*> widget) {
}

rpl::lifetime TakeWheel(
		not_null<QWidget*> widget,
		Fn<bool()> wanted,
		Fn<void(Wheel)> take) {
	return rpl::lifetime();
}

} // namespace Svipe::WebTabNative
