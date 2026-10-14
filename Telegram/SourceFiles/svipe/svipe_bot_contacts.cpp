/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bot_contacts.h"

#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "svipe/svipe_storage.h"

#include <QtCore/QJsonArray>

namespace Svipe::BotContacts {
namespace {

// The Android client keeps <botId>_requested / <botId>_granted per account; one object per bot here.
[[nodiscard]] QString Key(not_null<UserData*> bot) {
	return u"svipe_botcontacts_"_q + QString::number(bot->id.value);
}

[[nodiscard]] QJsonObject State(not_null<UserData*> bot) {
	return Storage::Get(&bot->session(), Key(bot)).toObject();
}

rpl::event_stream<uint64> &ChangesStream() {
	static auto result = rpl::event_stream<uint64>();
	return result;
}

} // namespace

bool Asked(not_null<UserData*> bot) {
	return State(bot).value(u"requested"_q).toBool();
}

bool Granted(not_null<UserData*> bot) {
	return State(bot).value(u"granted"_q).toBool();
}

void SetGranted(not_null<UserData*> bot, bool granted) {
	Storage::Set(&bot->session(), Key(bot), QJsonObject{
		{ u"requested"_q, true },
		{ u"granted"_q, granted },
	});
	ChangesStream().fire_copy(bot->id.value);
}

QJsonObject CheckObject(not_null<UserData*> bot) {
	auto result = QJsonObject{
		{ u"available"_q, true },
		{ u"access_requested"_q, Asked(bot) },
	};
	if (Asked(bot)) {
		result.insert(u"access_granted"_q, Granted(bot));
	}
	return result;
}

QJsonObject ContactsObject(not_null<UserData*> bot) {
	if (!Granted(bot)) {
		return QJsonObject{ { u"status"_q, u"cancelled"_q } };
	}
	auto list = QJsonArray();
	const auto contacts = bot->owner().contactsList();
	for (const auto &row : contacts->all()) {
		const auto history = row->history();
		const auto user = history ? history->peer->asUser() : nullptr;
		if (!user || user->isSelf() || user->isInaccessible()) {
			continue;
		}
		auto item = QJsonObject{
			{ u"user_id"_q, qint64(peerToUser(user->id).bare) },
			{ u"first_name"_q, user->firstName },
		};
		if (!user->lastName.isEmpty()) {
			item.insert(u"last_name"_q, user->lastName);
		}
		if (const auto username = user->username(); !username.isEmpty()) {
			item.insert(u"username"_q, username);
		}
		if (const auto phone = user->phone(); !phone.isEmpty()) {
			item.insert(u"phone_number"_q, phone);
		}
		list.push_back(item);
	}
	return QJsonObject{
		{ u"status"_q, u"allowed"_q },
		{ u"contacts"_q, list },
	};
}

rpl::producer<uint64> Changes() {
	return ChangesStream().events();
}

} // namespace Svipe::BotContacts
