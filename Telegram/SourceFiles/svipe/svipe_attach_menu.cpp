/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_attach_menu.h"

#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "svipe/svipe_storage.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QJsonArray>

namespace Svipe::AttachMenu {
namespace {

// The Android client keeps the same two lists in SharedPreferences.
const auto kKey = u"svipe_attachmenu"_q;

[[nodiscard]] QJsonObject State(not_null<Main::Session*> session) {
	return Storage::Get(session, kKey).toObject();
}

[[nodiscard]] std::vector<uint64> Ids(
		not_null<Main::Session*> session,
		const QString &list) {
	auto result = std::vector<uint64>();
	for (const auto &value : State(session).value(list).toArray()) {
		result.push_back(uint64(value.toString().toULongLong()));
	}
	return result;
}

void Save(
		not_null<Main::Session*> session,
		const std::vector<uint64> &bots,
		const std::vector<uint64> &declined) {
	const auto pack = [](const std::vector<uint64> &ids) {
		auto result = QJsonArray();
		for (const auto id : ids) {
			result.push_back(QString::number(id));
		}
		return result;
	};
	Storage::Set(session, kKey, QJsonObject{
		{ u"bots"_q, pack(bots) },
		{ u"declined"_q, pack(declined) },
	});
}

rpl::event_stream<> &ChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] uint64 Id(not_null<UserData*> bot) {
	return bot->id.value;
}

void SetState(not_null<UserData*> bot, bool added) {
	const auto session = &bot->session();
	auto bots = Ids(session, u"bots"_q);
	auto declined = Ids(session, u"declined"_q);
	const auto id = Id(bot);
	bots.erase(ranges::remove(bots, id), end(bots));
	declined.erase(ranges::remove(declined, id), end(declined));
	(added ? bots : declined).push_back(id);
	Save(session, bots, declined);
	ChangesStream().fire({});
}

} // namespace

std::vector<not_null<UserData*>> Bots(not_null<Main::Session*> session) {
	auto result = std::vector<not_null<UserData*>>();
	for (const auto id : Ids(session, u"bots"_q)) {
		if (const auto user = session->data().userLoaded(peerToUser(PeerId(id)))) {
			result.push_back(user);
		}
	}
	return result;
}

bool Contains(not_null<UserData*> bot) {
	return ranges::contains(Ids(&bot->session(), u"bots"_q), Id(bot));
}

QString Status(not_null<UserData*> bot) {
	if (Contains(bot)) {
		return u"added"_q;
	}
	return ranges::contains(Ids(&bot->session(), u"declined"_q), Id(bot))
		? u"declined"_q
		: u"missed"_q;
}

bool CanAdd(not_null<UserData*> bot) {
	const auto info = bot->botInfo.get();
	return info && (info->hasMainApp || !info->botMenuButtonUrl.isEmpty());
}

void Request(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		bool fromMiniApp,
		Fn<void(bool shownNow, bool added)> done) {
	if (Contains(bot)) {
		done(false, true);
		return;
	} else if ((fromMiniApp && Status(bot) == u"declined"_q) || !CanAdd(bot)) {
		done(false, false);
		return;
	}
	const auto answered = std::make_shared<bool>(false);
	const auto answer = [=](bool added) {
		if (*answered) {
			return;
		}
		*answered = true;
		SetState(bot, added);
		if (added) {
			show->showToast(tr::lng_bot_add_to_menu_done(tr::now));
		}
		done(true, added);
	};
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		Ui::ConfirmBox(box, {
			.text = tr::lng_bot_add_to_menu(
				tr::now,
				lt_bot,
				tr::bold(bot->name()),
				tr::marked),
			.confirmed = [=](Fn<void()> close) {
				answer(true);
				close();
			},
			.cancelled = [=](Fn<void()> close) {
				answer(false);
				close();
			},
		});
		box->boxClosing() | rpl::on_next([=] {
			answer(false);
		}, box->lifetime());
	}));
}

void Remove(std::shared_ptr<Ui::Show> show, not_null<UserData*> bot) {
	if (!Contains(bot)) {
		return;
	}
	SetState(bot, false);
	show->showToast(tr::lng_bot_remove_from_menu_done(tr::now));
}

rpl::producer<> Changes() {
	return ChangesStream().events();
}

} // namespace Svipe::AttachMenu
