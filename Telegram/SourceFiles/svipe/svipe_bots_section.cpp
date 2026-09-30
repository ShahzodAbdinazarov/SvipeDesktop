/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bots_section.h"

#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_mute.h"
#include "settings/settings_common.h"
#include "svipe/svipe_bot_mute.h"
#include "svipe/svipe_message_types.h"
#include "svipe/svipe_message_types_box.h"
#include "svipe/svipe_strings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

[[nodiscard]] uint64 BareId(not_null<PeerData*> peer) {
	return peerToUser(peer->id).bare;
}

// Select Chat, narrowed to bots that are not on the list yet — a person picked here would be a
// person added to a list about bots.
class BotPickerController final : public ChatsListBoxController {
public:
	BotPickerController(
		not_null<::Main::Session*> session,
		Fn<void(not_null<UserData*>)> done)
	: ChatsListBoxController(session)
	, _session(session)
	, _done(std::move(done)) {
	}

	::Main::Session &session() const override {
		return *_session;
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		if (const auto user = row->peer()->asUser()) {
			if (_done) {
				_done(user);
			}
		}
	}

private:
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::lng_notification_exceptions_add());
	}

	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		const auto user = history->peer->asUser();
		if (!user
			|| !user->isBot()
			|| ranges::contains(
				Svipe::BotMute::Exceptions(_session),
				BareId(user))) {
			return nullptr;
		}
		return std::make_unique<Row>(history);
	}

	const not_null<::Main::Session*> _session;
	const Fn<void(not_null<UserData*>)> _done;

};

// The bots on the list, the way the exceptions of Private Chats are listed: userpic, name, status,
// a Remove link, and the chat's own menu on click.
class BotExceptionsController final : public PeerListController {
public:
	explicit BotExceptionsController(
		not_null<Window::SessionController*> window)
	: _window(window) {
	}

	::Main::Session &session() const override {
		return _window->session();
	}

	void prepare() override {
		refreshRows();
		Svipe::BotMute::Changes(
		) | rpl::on_next([=] {
			refreshRows();
		}, _lifetime);
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListShowRowMenu(row, true);
	}

	void rowRightActionClicked(not_null<PeerListRow*> row) override {
		Svipe::BotMute::SetException(&session(), BareId(row->peer()), false);
	}

	base::unique_qptr<Ui::PopupMenu> rowContextMenu(
			QWidget *parent,
			not_null<PeerListRow*> row) override {
		const auto peer = row->peer();
		auto result = base::make_unique_q<Ui::PopupMenu>(
			parent,
			st::popupMenuWithIcons);
		result->addAction(
			tr::lng_context_view_profile(tr::now),
			crl::guard(_window, [window = _window.get(), peer] {
				window->showPeerInfo(peer);
			}),
			&st::menuIconProfile);
		result->addSeparator();
		MuteMenu::FillMuteMenu(
			result.get(),
			peer->owner().history(peer),
			_window->uiShow());
		base::take(_menu);
		_menu = base::unique_qptr<Ui::PopupMenu>(result.get());
		return result;
	}

	[[nodiscard]] rpl::producer<int> countValue() const {
		return _count.value();
	}

private:
	void refreshRows() {
		const auto ids = Svipe::BotMute::Exceptions(&session());
		for (auto i = 0; i != delegate()->peerListFullRowsCount();) {
			const auto row = delegate()->peerListRowAt(i);
			if (ranges::contains(ids, BareId(row->peer()))) {
				++i;
			} else {
				delegate()->peerListRemoveRow(row);
			}
		}
		for (const auto id : ids) {
			const auto user = session().data().userLoaded(UserId(id));
			if (user && !delegate()->peerListFindRow(user->id.value)) {
				auto row = std::make_unique<PeerListRowWithLink>(user);
				row->setActionLink(tr::lng_notification_exceptions_remove(tr::now));
				row->setCustomStatus(Svipe::Tr(Svipe::Str::NotificationsBotOn));
				delegate()->peerListAppendRow(std::move(row));
			}
		}
		delegate()->peerListRefreshRows();
		_count = delegate()->peerListFullRowsCount();
	}

	const not_null<Window::SessionController*> _window;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::variable<int> _count;
	rpl::lifetime _lifetime;

};

void SetupSwitch(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	Ui::AddSubsectionTitle(
		container,
		Svipe::TrValue(Svipe::Str::NotificationsBotsTitle));
	const auto enabled = AddButtonWithIcon(
		container,
		tr::lng_notification_enable(),
		st::settingsButton,
		{ &st::menuIconNotifications });
	enabled->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		Svipe::BotMute::Changes()
	) | rpl::map([=] {
		return !Svipe::BotMute::IsEnabled(session);
	}));
	enabled->toggledChanges(
	) | rpl::filter([=](bool notify) {
		return notify == Svipe::BotMute::IsEnabled(session);
	}) | rpl::on_next([=](bool notify) {
		Svipe::BotMute::SetEnabled(session, !notify);
	}, enabled->lifetime());
}

void SetupExceptions(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	Ui::AddSubsectionTitle(
		container,
		Svipe::TrValue(Svipe::Str::NotificationsBotsExceptions));
	const auto add = AddButtonWithIcon(
		container,
		tr::lng_notification_exceptions_add(),
		st::settingsButtonActive,
		{ &st::menuIconInviteSettings });

	auto controller = std::make_unique<BotExceptionsController>(window);
	controller->setStyleOverrides(&st::settingsBlockedList);
	const auto content = container->add(
		object_ptr<PeerListContent>(container, controller.get()));

	struct State {
		std::unique_ptr<BotExceptionsController> controller;
		std::unique_ptr<PeerListContentDelegateSimple> delegate;
	};
	const auto state = content->lifetime().make_state<State>();
	state->controller = std::move(controller);
	state->delegate = std::make_unique<PeerListContentDelegateSimple>();
	state->delegate->setContent(content);
	state->controller->setDelegate(state->delegate.get());

	add->setClickedCallback([=] {
		const auto box = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
		const auto done = [=](not_null<UserData*> user) {
			Svipe::BotMute::SetException(session, BareId(user), true);
			if (*box) {
				(*box)->closeBox();
			}
		};
		auto picker = std::make_unique<BotPickerController>(
			session,
			crl::guard(content, done));
		auto initBox = [=](not_null<PeerListBox*> box) {
			box->addButton(tr::lng_cancel(), [box] { box->closeBox(); });
		};
		*box = window->show(
			Box<PeerListBox>(std::move(picker), std::move(initBox)));
	});

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			container,
			CreateButtonWithIcon(
				container,
				tr::lng_notification_exceptions_clear(),
				st::settingsAttentionButtonWithIcon,
				{ &st::menuIconDeleteAttention })));
	wrap->entity()->setClickedCallback([=] {
		const auto clear = [=](Fn<void()> close) {
			for (const auto id : Svipe::BotMute::Exceptions(session)) {
				Svipe::BotMute::SetException(session, id, false);
			}
			close();
		};
		window->show(Ui::MakeConfirmBox({
			.text = tr::lng_notification_exceptions_clear_sure(),
			.confirmed = clear,
			.confirmText = tr::lng_notification_exceptions_clear_button(),
			.confirmStyle = &st::attentionBoxButton,
			.title = tr::lng_notification_exceptions_clear(),
		}));
	});
	wrap->toggleOn(
		state->controller->countValue() | rpl::map(rpl::mappers::_1 > 0),
		anim::type::instant);
}

} // namespace

SvipeNotificationsBots::SvipeNotificationsBots(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent(controller);
}

rpl::producer<QString> SvipeNotificationsBots::title() {
	return Svipe::TrValue(Svipe::Str::NotificationsBots);
}

void SvipeNotificationsBots::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto container = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto session = &controller->session();

	Ui::AddSkip(container, st::settingsPrivacySkip);
	SetupSwitch(container, controller);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, Svipe::TrValue(Svipe::Str::NotificationsBotsInfo));
	Ui::AddSkip(container);

	AddButtonWithIcon(
		container,
		Svipe::TrValue(Svipe::Str::MessageTypes),
		st::settingsButton,
		{ &st::menuIconCustomize }
	)->setClickedCallback([=] {
		controller->show(Box(
			Svipe::MessageTypesScopeBox,
			session,
			Svipe::MessageTypes::kScopeBots,
			Svipe::Str::MessageTypes));
	});
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);

	SetupExceptions(container, controller);
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		Svipe::TrValue(Svipe::Str::NotificationsBotsExceptionsInfo));

	Ui::ResizeFitChild(this, container);
}

} // namespace Settings
