/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bots_box.h"

#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "svipe/svipe_bot_mute.h"
#include "svipe/svipe_message_types.h"
#include "svipe/svipe_message_types_box.h"
#include "svipe/svipe_strings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Svipe {
namespace {

// The Select Chat list Telegram opens from Add Exception, narrowed to bots that are not on the list
// yet — a person picked here would be a person added to a list about bots.
class BotPickerController final : public ChatsListBoxController {
public:
	BotPickerController(
		not_null<Main::Session*> session,
		Fn<void(not_null<UserData*>)> done)
	: ChatsListBoxController(session)
	, _session(session)
	, _done(std::move(done)) {
	}

	Main::Session &session() const override {
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
		if (!user || !user->isBot()) {
			return nullptr;
		}
		const auto except = BotMute::Exceptions(_session);
		if (ranges::contains(except, peerToUser(user->id).bare)) {
			return nullptr;
		}
		return std::make_unique<Row>(history);
	}

	const not_null<Main::Session*> _session;
	const Fn<void(not_null<UserData*>)> _done;

};

void FillExceptions(
		not_null<Ui::VerticalLayout*> container,
		not_null<Main::Session*> session) {
	container->clear();
	for (const auto id : BotMute::Exceptions(session)) {
		const auto user = session->data().userLoaded(UserId(id));
		if (!user) {
			continue;
		}
		const auto button = Settings::AddButtonWithLabel(
			container,
			rpl::single(user->name()),
			TrValue(Str::NotificationsBotOn),
			st::settingsButton,
			{ &st::menuIconBot });
		button->setClickedCallback([=] {
			// The row asks before it acts, like the exceptions next door.
			const auto menu = Ui::CreateChild<Ui::PopupMenu>(
				button,
				st::popupMenuWithIcons);
			menu->addAction(
				tr::lng_notification_exceptions_remove(tr::now),
				[=] { BotMute::SetException(session, id, false); },
				&st::menuIconRemove);
			menu->popup(QCursor::pos());
		});
	}
	container->resizeToWidth(container->width());
}

} // namespace

rpl::producer<QString> BotsRowLabel(not_null<Main::Session*> session) {
	return rpl::single(
		rpl::empty
	) | rpl::then(rpl::merge(
		BotMute::Changes(),
		Lang::GetInstance().updated()
	)) | rpl::map([=] {
		if (!BotMute::IsEnabled(session)) {
			return Tr(Str::NotificationsBotsOn);
		}
		const auto count = int(BotMute::Exceptions(session).size());
		return count
			? BotsExceptionsCount(count)
			: Tr(Str::NotificationsBotsOff);
	});
}

void BotsNotificationsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	box->setTitle(TrValue(Str::NotificationsBots));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();

	Ui::AddSubsectionTitle(container, tr::lng_notification_enable());
	const auto toggle = Settings::AddButtonWithIcon(
		container,
		TrValue(Str::NotificationsBotsMessages),
		st::settingsButton,
		{ &st::menuIconBot });
	toggle->toggleOn(rpl::single(!BotMute::IsEnabled(session)));
	toggle->toggledChanges(
	) | rpl::on_next([=](bool notify) {
		BotMute::SetEnabled(session, !notify);
	}, toggle->lifetime());

	Settings::AddButtonWithIcon(
		container,
		TrValue(Str::MessageTypes),
		st::settingsButton,
		{ &st::menuIconCustomize }
	)->setClickedCallback([=] {
		controller->show(Box(
			MessageTypesScopeBox,
			session,
			MessageTypes::kScopeBots,
			Str::MessageTypes));
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(Str::NotificationsBotsInfo));
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, TrValue(Str::NotificationsBotsExceptions));
	const auto add = Settings::AddButtonWithIcon(
		container,
		tr::lng_notification_exceptions_add(),
		st::settingsButtonActive,
		{ &st::menuIconAdd });
	const auto list = container->add(
		object_ptr<Ui::VerticalLayout>(container));
	const auto clear = Settings::AddButtonWithIcon(
		container,
		tr::lng_notification_exceptions_clear(),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconDeleteAttention });
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		TrValue(Str::NotificationsBotsExceptionsInfo));

	const auto refresh = [=] {
		FillExceptions(list, session);
		clear->setVisible(!BotMute::Exceptions(session).empty());
	};
	refresh();
	BotMute::Changes(
	) | rpl::on_next(refresh, list->lifetime());

	add->setClickedCallback([=] {
		const auto picker = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
		auto pickerController = std::make_unique<BotPickerController>(
			session,
			crl::guard(list, [=](not_null<UserData*> user) {
				BotMute::SetException(session, peerToUser(user->id).bare, true);
				if (*picker) {
					(*picker)->closeBox();
				}
			}));
		auto initBox = [=](not_null<PeerListBox*> box) {
			box->addButton(tr::lng_cancel(), [box] { box->closeBox(); });
		};
		*picker = controller->show(Box<PeerListBox>(
			std::move(pickerController),
			std::move(initBox)));
	});
	clear->setClickedCallback([=] {
		for (const auto id : BotMute::Exceptions(session)) {
			BotMute::SetException(session, id, false);
		}
	});

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Svipe
