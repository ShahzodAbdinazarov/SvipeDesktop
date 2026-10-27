/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_web_tab.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "core/file_utilities.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "info/profile/tabs/info_profile_tabs_host.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "svipe/svipe_attach_menu.h"
#include "svipe/svipe_web_tab_native.h"
#include "ui/basic_click_handlers.h"
#include "webview/webview_embed.h"
#include "webview/webview_interface.h"
#include "window/themes/window_theme.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QApplication>

namespace Svipe::WebTab {
namespace {

constexpr auto kPlaceInterval = crl::time(100);

// The page posts its events here, as Telegram's script does in any mini app.
constexpr auto kProxyScript = R"(
window.TelegramWebviewProxy = {
postEvent: function(eventType, eventData) {
	if (window.external && window.external.invoke) {
		window.external.invoke(JSON.stringify([eventType, eventData]));
	}
}
};)";

// Where the app can't take the wheel from the web view (Windows, Linux), the page hands it over while
// the profile still has to scroll: the app tells it when with __svipeTabHandWheel(true|false).
constexpr auto kWheelScript = R"(
(function() {
	var hand = false;
	window.__svipeTabHandWheel = function(value) { hand = !!value; };
	window.addEventListener('wheel', function(e) {
		if (!hand || !window.external || !window.external.invoke) {
			return;
		}
		e.preventDefault();
		e.stopImmediatePropagation();
		window.external.invoke(JSON.stringify(['svipe_tab_wheel', {
			dx: e.deltaX, dy: e.deltaY, mode: e.deltaMode,
			shift: e.shiftKey, ctrl: e.ctrlKey, alt: e.altKey, meta: e.metaKey
		}]));
	}, { capture: true, passive: false });
})();)";

} // namespace

struct View::Private {
	// Native and a child of the window, outside the profile's scroll area: it is placed over the tab's
	// visible part on every scroll and cuts the page, which stays as tall as the tab, to it.
	base::unique_qptr<Ui::RpWidget> host;
	std::unique_ptr<Webview::Window> webview;
	QString url;
	base::Timer timer;
	rpl::lifetime wheel;
	bool requested = false;
};

View::View(QWidget *parent, not_null<Main::Session*> session, Args args)
: RpWidget(parent)
, _session(session)
, _args(std::move(args))
, _private(std::make_unique<Private>()) {
	_private->timer.setCallback([=] { place(); });
	_private->timer.callEach(kPlaceInterval);
	load();
}

View::~View() {
	_private->wheel.destroy();
	_private->webview = nullptr;
	_private->host = nullptr;
}

void View::setVisibleRegion(int top, int bottom) {
	const auto heightChanged = (bottom - top) != (_visibleBottom - _visibleTop);
	_visibleTop = top;
	_visibleBottom = bottom;
	if (heightChanged) {
		resizeToWidth(width());
	}
	place();
}

void View::setTopOverlay(int height) {
	_overlay = height;
	place();
}

void View::setActive(bool active) {
	_active = active;
	place();
}

int View::pageHeight() const {
	// As tall as the tab's page under the pinned tabs, as Android's web cell.
	auto strip = _overlay;
	for (auto parent = parentWidget(); parent; parent = parent->parentWidget()) {
		if (const auto host = dynamic_cast<Info::Profile::TabsHost*>(parent)) {
			strip = host->stripWidget()->height();
			break;
		}
	}
	const auto span = _visibleBottom - _visibleTop;
	return std::max(span - strip, st::defaultPeerListItem.height * 4);
}

int View::resizeGetHeight(int newWidth) {
	return pageHeight();
}

bool View::profileFullyScrolled() const {
	// The tab's page has reached the bottom of the profile's view: the tabs are pinned to the top.
	return (_visibleBottom >= height() - 1);
}

void View::load() {
	const auto bot = _session->data().userLoaded(UserId(_args.botId));
	if (bot && bot->isBot()) {
		open(bot);
		return;
	} else if (_args.botUsername.isEmpty()) {
		return;
	}
	const auto weak = base::make_weak(this);
	_session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(_args.botUsername),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		const auto peer = _session->data().peerLoaded(peerFromMTP(data.vpeer()));
		if (const auto user = peer ? peer->asUser() : nullptr; user && weak) {
			open(user);
		}
	}).send();
}

void View::open(not_null<UserData*> bot) {
	if (std::exchange(_private->requested, true)) {
		return;
	}
	// requestWebView, as an inline button in the bot's chat opens it: the page needs Telegram's
	// initData to know who is looking.
	using Flag = MTPmessages_RequestWebView::Flag;
	const auto weak = base::make_weak(this);
	_session->api().request(MTPmessages_RequestWebView(
		MTP_flags(Flag::f_url | Flag::f_theme_params),
		bot->input(),
		bot->inputUser(),
		MTP_string(_args.url),
		MTPstring(), // start_param
		MTP_dataJSON(MTP_bytes(Window::Theme::WebViewParams().json)),
		MTP_string("tdesktop"),
		MTPInputReplyTo(),
		MTPInputPeer() // send_as
	)).done([=](const MTPWebViewResult &result) {
		if (!weak) {
			return;
		}
		LOG(("Svipe WebTab: page for bot %1 opened.").arg(_args.botId));
		const auto url = result.match([](const MTPDwebViewResultUrl &data) {
			return qs(data.vurl());
		});
		const auto peer = _session->data().peerLoaded(_args.peer);
		const auto navigate = crl::guard(this, [=](QString signedUrl) {
			_private->url = signedUrl;
			createHost();
			if (_private->webview) {
				_private->webview->navigate(signedUrl);
			}
			LOG(("Svipe WebTab: navigate, webview: %1, signed: %2"
				).arg(Logs::b(_private->webview != nullptr)
				).arg(Logs::b(signedUrl.contains(u"tgWebAppSvipeData"_q))));
			place();
		});
		if (!peer) {
			navigate(url);
			return;
		}
		// svipeInitData for whoever is looking, whatever the tab shows.
		Svipe::AttachMenu::SignContext(
			bot,
			peer,
			url,
			QString(),
			navigate,
			_args.chatType);
	}).fail([=](const MTP::Error &error) {
		LOG(("Svipe WebTab: requestWebView failed: %1").arg(error.type()));
	}).send();
}

void View::createHost() {
	if (_private->host) {
		return;
	}
	const auto window = this->window();
	_private->host = base::make_unique_q<Ui::RpWidget>(window);
	const auto host = _private->host.get();
	host->setAttribute(Qt::WA_NativeWindow);
	host->setAttribute(Qt::WA_DontCreateNativeAncestors);
	host->hide();
	WebTabNative::ClipToBounds(host);

	const auto params = Window::Theme::WebViewParams();
	_private->webview = std::make_unique<Webview::Window>(
		host,
		Webview::WindowConfig{
			.opaqueBg = params.bodyBg,
			.storageId = _session->local().resolveStorageIdBots(),
		});
	const auto raw = _private->webview.get();
	if (!raw->widget()) {
		LOG(("Svipe WebTab: no web view widget."));
		_private->webview = nullptr;
		return;
	}
	raw->updateTheme(
		params.bodyBg,
		params.scrollBg,
		params.scrollBgOver,
		params.scrollBarBg,
		params.scrollBarBgOver);
	raw->init(QByteArray(kProxyScript));
	if (!WebTabNative::TakesWheel()) {
		raw->init(QByteArray(kWheelScript));
		raw->setNavigationDoneHandler(crl::guard(this, [=](bool) {
			pushPageTakesWheel(true);
		}));
	}
	raw->setMessageHandler([=](const QJsonDocument &message) {
		const auto list = message.array();
		if (list.isEmpty()) {
			return;
		}
		const auto command = list.at(0).toString();
		auto arguments = QJsonObject();
		const auto data = list.at(1);
		if (data.isObject()) {
			arguments = data.toObject();
		} else if (data.isString()) {
			arguments = QJsonDocument::fromJson(data.toString().toUtf8()).object();
		}
		crl::on_main(this, [=] {
			handle(command, arguments);
		});
	});
	raw->setNavigationPolicyHandler([=](QString uri, bool newWindow) {
		if (newWindow) {
			crl::on_main(this, [=] {
				UrlClickHandler::Open(uri);
			});
			return false;
		}
		return true;
	});

	// Until the tabs are pinned to the top the profile scrolls (and collapses its header) as with any
	// tab; from then on the page scrolls itself.
	const auto weak = base::make_weak(this);
	_private->wheel = WebTabNative::TakeWheel(host, [=] {
		return weak && !profileFullyScrolled();
	}, [=](WebTabNative::Wheel wheel) {
		if (weak) {
			sendWheel(wheel.pixelDelta, wheel.angleDelta, wheel.modifiers);
		}
	});
}

void View::sendWheel(
		QPoint pixelDelta,
		QPoint angleDelta,
		Qt::KeyboardModifiers modifiers) {
	auto scroll = static_cast<QAbstractScrollArea*>(nullptr);
	for (auto parent = parentWidget(); parent; parent = parent->parentWidget()) {
		if ((scroll = dynamic_cast<QAbstractScrollArea*>(parent))) {
			break;
		}
	}
	if (!scroll) {
		return;
	}
	const auto viewport = scroll->viewport();
	const auto global = QCursor::pos();
	auto event = QWheelEvent(
		viewport->mapFromGlobal(global),
		global,
		pixelDelta,
		angleDelta,
		Qt::NoButton,
		modifiers,
		Qt::NoScrollPhase,
		false);
	QCoreApplication::sendEvent(viewport, &event);
	pushPageTakesWheel();
}

void View::pushPageTakesWheel(bool force) {
	if (WebTabNative::TakesWheel() || !_private->webview) {
		return;
	}
	const auto hand = !profileFullyScrolled();
	if (!force && _pageHandsWheel == hand) {
		return;
	}
	_pageHandsWheel = hand;
	_private->webview->eval(hand
		? "window.__svipeTabHandWheel && window.__svipeTabHandWheel(true);"
		: "window.__svipeTabHandWheel && window.__svipeTabHandWheel(false);");
}

bool View::covered(QRect global) const {
	// A box or another layer drawn by Qt over the profile: the native page would show through it.
	// What belongs to the profile itself (its scroll bar, shadows, the pinned tabs) doesn't count.
	const auto window = this->window();
	auto section = parentWidget();
	for (auto parent = parentWidget(); parent; parent = parent->parentWidget()) {
		if (dynamic_cast<QAbstractScrollArea*>(parent)) {
			section = parent->parentWidget() ? parent->parentWidget() : parent;
			break;
		}
	}
	const auto host = _private->host.get();
	const auto top = window->childAt(window->mapFromGlobal(global.center()));
	if (!top || top == host || (host && host->isAncestorOf(top))) {
		return false;
	}
	return !(section && (section == top || section->isAncestorOf(top)));
}

void View::place() {
	const auto host = _private->host.get();
	if (!host) {
		return;
	}
	pushPageTakesWheel();
	const auto window = this->window();
	const auto hide = [&] {
		if (!host->isHidden()) {
			host->hide();
		}
	};
	if (!_active
		|| !isVisible()
		|| !window->isVisible()
		|| window->isMinimized()
		|| width() <= 0) {
		hide();
		return;
	}
	// The part of the tab's page under the pinned tabs and inside the profile's view.
	const auto from = std::max(0, _visibleTop + _overlay);
	const auto till = std::min(height(), _visibleBottom);
	if (till <= from) {
		hide();
		return;
	}
	const auto local = QRect(mapTo(window, QPoint(0, from)), QSize(width(), till - from));
	const auto global = QRect(window->mapToGlobal(local.topLeft()), local.size());
	// A layer drawn by Qt over the tab (a box, a menu panel) would be covered by the native page: the
	// page steps away until it goes.
	if (covered(global)) {
		hide();
		return;
	}
	host->setGeometry(local);
	if (const auto view = _private->webview ? _private->webview->widget() : nullptr) {
		view->setGeometry(0, -from, width(), height());
	}
	if (host->isHidden()) {
		host->show();
		host->raise();
	}
}

void View::handle(const QString &command, const QJsonObject &arguments) {
	if (command == u"svipe_tab_wheel"_q) {
		// The page handed the wheel over (svipe_tab_wheel): the profile scrolls. A wheel's notch is
		// 100 px in the page and 120 in Qt; a line, 40 px.
		const auto mode = arguments.value(u"mode"_q).toInt();
		const auto scale = (mode == 1) ? 40. : (mode == 2) ? 400. : 1.;
		const auto dx = arguments.value(u"dx"_q).toDouble() * scale;
		const auto dy = arguments.value(u"dy"_q).toDouble() * scale;
		auto modifiers = Qt::KeyboardModifiers();
		if (arguments.value(u"shift"_q).toBool()) modifiers |= Qt::ShiftModifier;
		if (arguments.value(u"ctrl"_q).toBool()) modifiers |= Qt::ControlModifier;
		if (arguments.value(u"alt"_q).toBool()) modifiers |= Qt::AltModifier;
		sendWheel(
			QPoint(),
			QPoint(qRound(-dx * 1.2), qRound(-dy * 1.2)),
			modifiers);
		return;
	} else if (command == u"web_app_request_theme"_q) {
		postEvent(u"theme_changed"_q, QJsonObject{
			{ u"theme_params"_q, QJsonDocument::fromJson(
				Window::Theme::WebViewParams().json).object() },
		});
	} else if (command == u"web_app_request_viewport"_q) {
		postEvent(u"viewport_changed"_q, QJsonObject{
			{ u"height"_q, height() },
			{ u"is_state_stable"_q, true },
			{ u"is_expanded"_q, true },
		});
	} else if (command == u"web_app_request_safe_area"_q) {
		postEvent(u"safe_area_changed"_q, QJsonObject{
			{ u"top"_q, 0 }, { u"bottom"_q, 0 }, { u"left"_q, 0 }, { u"right"_q, 0 },
		});
	} else if (command == u"web_app_request_content_safe_area"_q) {
		postEvent(u"content_safe_area_changed"_q, QJsonObject{
			{ u"top"_q, 0 }, { u"bottom"_q, 0 }, { u"left"_q, 0 }, { u"right"_q, 0 },
		});
	} else if (command == u"web_app_open_link"_q) {
		const auto url = arguments.value(u"url"_q).toString();
		if (url.startsWith(u"https://"_q) || url.startsWith(u"http://"_q)) {
			File::OpenUrl(url);
		}
	} else if (command == u"web_app_open_tg_link"_q) {
		const auto path = arguments.value(u"path_full"_q).toString();
		if (path.startsWith('/')) {
			UrlClickHandler::Open(u"https://t.me"_q + path);
		}
	}
	// The rest (main button, closing, haptics, ...) has no place in a tab, as on Android.
}

void View::postEvent(const QString &event, const QJsonObject &data) {
	if (!_private->webview) {
		return;
	}
	const auto written = data.isEmpty()
		? QByteArray()
		: QJsonDocument(data).toJson(QJsonDocument::Compact);
	_private->webview->eval(R"(if (window.TelegramGameProxy) {
window.TelegramGameProxy.receiveEvent(")"
		+ event.toUtf8()
		+ '"' + (written.isEmpty() ? QByteArray() : ", " + written)
		+ R"();
})");
}

} // namespace Svipe::WebTab
