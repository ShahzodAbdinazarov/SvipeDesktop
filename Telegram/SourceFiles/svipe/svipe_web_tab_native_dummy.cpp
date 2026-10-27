/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Not yet on this platform: the page shows unclipped and takes the scroll wheel itself.
*/
#include "svipe/svipe_web_tab_native.h"

namespace Svipe::WebTabNative {

void ClipToBounds(not_null<QWidget*> widget) {
}

rpl::lifetime TakeWheel(
		not_null<QWidget*> widget,
		Fn<bool()> wanted,
		Fn<void(Wheel)> take) {
	return rpl::lifetime();
}

} // namespace Svipe::WebTabNative
