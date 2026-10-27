/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_web_tab_native.h"

#include <QtWidgets/QWidget>

#import <AppKit/AppKit.h>

namespace Svipe::WebTabNative {
namespace {

[[nodiscard]] NSView *ViewOf(not_null<QWidget*> widget) {
	return reinterpret_cast<NSView*>(widget->winId());
}

[[nodiscard]] Qt::ScrollPhase PhaseOf(NSEvent *event) {
	if (event.momentumPhase != NSEventPhaseNone) {
		return Qt::ScrollMomentum;
	}
	switch (event.phase) {
	case NSEventPhaseBegan:
	case NSEventPhaseMayBegin: return Qt::ScrollBegin;
	case NSEventPhaseChanged:
	case NSEventPhaseStationary: return Qt::ScrollUpdate;
	case NSEventPhaseEnded:
	case NSEventPhaseCancelled: return Qt::ScrollEnd;
	default: return Qt::NoScrollPhase;
	}
}

[[nodiscard]] Qt::KeyboardModifiers ModifiersOf(NSEvent *event) {
	auto result = Qt::KeyboardModifiers();
	const auto flags = event.modifierFlags;
	if (flags & NSEventModifierFlagShift) result |= Qt::ShiftModifier;
	if (flags & NSEventModifierFlagControl) result |= Qt::MetaModifier;
	if (flags & NSEventModifierFlagOption) result |= Qt::AltModifier;
	if (flags & NSEventModifierFlagCommand) result |= Qt::ControlModifier;
	return result;
}

} // namespace

void ClipToBounds(not_null<QWidget*> widget) {
	const auto view = ViewOf(widget);
	view.clipsToBounds = YES;
	view.wantsLayer = YES;
	view.layer.masksToBounds = YES;
}

rpl::lifetime TakeWheel(
		not_null<QWidget*> widget,
		Fn<bool()> wanted,
		Fn<void(Wheel)> take) {
	const auto weak = QPointer<QWidget>(widget.get());
	id monitor = [NSEvent
		addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
		handler:^NSEvent*(NSEvent *event) {
			const auto raw = weak.data();
			if (!raw || !raw->isVisible() || !wanted()) {
				return event;
			}
			const auto view = ViewOf(raw);
			if (event.window != view.window) {
				return event;
			}
			const auto point = [view convertPoint:event.locationInWindow fromView:nil];
			if (!NSPointInRect(point, view.bounds)) {
				return event;
			}
			auto wheel = Wheel{
				.inverted = event.isDirectionInvertedFromDevice ? true : false,
				.phase = PhaseOf(event),
				.modifiers = ModifiersOf(event),
			};
			if (event.hasPreciseScrollingDeltas) {
				wheel.pixelDelta = QPoint(
					qRound(event.scrollingDeltaX),
					qRound(event.scrollingDeltaY));
				wheel.angleDelta = wheel.pixelDelta * 2;
			} else {
				wheel.angleDelta = QPoint(
					qRound(event.scrollingDeltaX * 120),
					qRound(event.scrollingDeltaY * 120));
			}
			take(wheel);
			return nil;
		}];
	auto result = rpl::lifetime();
	result.add([=] {
		[NSEvent removeMonitor:monitor];
	});
	return result;
}

} // namespace Svipe::WebTabNative
