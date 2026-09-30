/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_message_types_box.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "svipe/svipe_message_types.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Svipe {
namespace {

void AddList(
		not_null<Ui::VerticalLayout*> container,
		not_null<Main::Session*> session,
		bool muted,
		QString target) {
	Ui::AddSubsectionTitle(
		container,
		TrValue(muted ? Str::MutedTypesHeader : Str::UnmutedTypesHeader));
	for (const auto &kind : MessageTypes::Kinds()) {
		const auto button = Settings::AddButtonWithIcon(
			container,
			TrValue(MessageTypes::LabelOf(kind)),
			st::settingsButton,
			{ MessageTypes::IconOf(kind) });
		button->toggleOn(rpl::single(
			MessageTypes::Has(session, muted, kind, target)));
		button->toggledChanges(
		) | rpl::on_next([=](bool on) {
			MessageTypes::Set(session, muted, kind, target, on);
		}, button->lifetime());
	}
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		TrValue(muted ? Str::MutedTypesInfo : Str::UnmutedTypesInfo));
}

} // namespace

void MessageTypesScopeBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		QString scope,
		Str title) {
	box->setTitle(TrValue(title));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	AddList(container, session, true, scope);
	Ui::AddSkip(container);
	AddList(container, session, false, scope);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void MessageTypesChatBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer) {
	const auto session = &peer->session();
	const auto muted = peer->owner().notifySettings().isMuted(peer);
	box->setTitle(TrValue(Str::MessageTypes));
	box->setWidth(st::boxWideWidth);
	// A chat that rings gets the kinds to silence; a muted chat gets the kinds to let through.
	AddList(box->verticalLayout(), session, !muted, MessageTypes::TargetOf(peer));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Svipe
