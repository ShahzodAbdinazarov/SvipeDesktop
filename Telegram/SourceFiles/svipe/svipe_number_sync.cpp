/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_number_sync.h"

#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_number_history.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QJsonArray>

namespace Svipe::NumberSync {
namespace {

// Android: SvipeNumberSync.RESYNC_AFTER_MS.
constexpr auto kResyncAfter = 5 * 60 * crl::time(1000);
constexpr auto kMaxContacts = 5000;

const auto kShareKey = u"svipe_number_sync_enabled"_q;

const auto kEveryone = u"everyone"_q;
const auto kContacts = u"contacts"_q;
const auto kNobody = u"nobody"_q;
const auto kOff = u"off"_q;

base::flat_map<uint64, crl::time> &LastSynced() {
	static auto result = base::flat_map<uint64, crl::time>();
	return result;
}

// On by default, like Telegram's own privacy settings (SvipeConfig.isNumberSyncEnabled).
[[nodiscard]] bool ShareEnabled(not_null<Main::Session*> session) {
	const auto value = Storage::Get(session, kShareKey);
	return value.isUndefined() || value.toBool();
}

[[nodiscard]] QString Iso(int64 ms) {
	return QDateTime::fromMSecsSinceEpoch(
		ms > 0 ? ms : QDateTime::currentMSecsSinceEpoch(),
		QTimeZone::UTC).toString(u"yyyy-MM-ddTHH:mm:ssZ"_q);
}

// The server speaks ISO-8601; anything unparseable becomes "now", which only widens a window.
[[nodiscard]] int64 ParseIso(const QString &value) {
	auto parsed = QDateTime::fromString(value.left(19), u"yyyy-MM-ddTHH:mm:ss"_q);
	if (!parsed.isValid()) {
		return 0;
	}
	parsed.setTimeZone(QTimeZone::UTC);
	return parsed.toMSecsSinceEpoch();
}

[[nodiscard]] QJsonObject Binding(
		uint64 userId,
		const QString &phone,
		int64 firstSeen,
		int64 lastSeen) {
	return QJsonObject{
		{ u"subject_tg_id"_q, qint64(userId) },
		{ u"phone"_q, phone },
		{ u"first_seen"_q, Iso(firstSeen) },
		{ u"last_seen"_q, Iso(lastSeen) },
	};
}

// What this device knows about the person, and about whoever else held their numbers.
void Contribute(not_null<Main::Session*> session, uint64 userId) {
	auto bindings = QJsonArray();
	for (const auto &number : NumberHistory::NumbersOfAccount(userId)) {
		bindings.push_back(Binding(
			userId,
			number.phone,
			number.firstSeen,
			number.lastSeen));
		for (const auto &seen : NumberHistory::AccountsOnNumber(number.phone)) {
			if (seen.userId != userId) {
				bindings.push_back(Binding(
					seen.userId,
					number.phone,
					seen.firstSeen,
					seen.lastSeen));
			}
		}
	}
	if (bindings.isEmpty()) {
		return;
	}
	Auth::EnsureToken(session, [=](QString token) {
		if (!token.isEmpty()) {
			Api::Post(
				u"/v1/numbers/observed"_q,
				{ { u"bindings"_q, bindings } },
				token,
				nullptr);
		}
	});
}

void Fetch(not_null<Main::Session*> session, uint64 userId) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(u"/v1/numbers/%1"_q.arg(userId), token, [=](
				QJsonObject result,
				int code) {
			if (code < 200 || code >= 300) {
				// 403 is the normal answer for someone we may not ask about; let a later open retry
				// anything else.
				if (code != 403) {
					LastSynced().remove(userId);
				}
				return;
			}
			// Numbers this account has left behind; the current one is never among them.
			for (const auto &value : result.value(u"old_numbers"_q).toArray()) {
				const auto o = value.toObject();
				NumberHistory::Merge(
					userId,
					o.value(u"phone"_q).toString(),
					ParseIso(o.value(u"first_seen"_q).toString()),
					ParseIso(o.value(u"last_seen"_q).toString()));
			}
			auto ids = std::vector<uint64>();
			for (const auto &value : result.value(u"old_profile_ids"_q).toArray()) {
				ids.push_back(uint64(value.toInteger()));
			}
			NumberHistory::StoreOldProfiles(userId, ids);
		});
	});
}

// The options in Telegram's own words, as the Android screen shows them.
[[nodiscard]] QString VisibilityLabel(const QString &value) {
	return (value == kContacts)
		? tr::lng_edit_privacy_contacts(tr::now)
		: (value == kNobody)
		? tr::lng_edit_privacy_nobody(tr::now)
		: (value == kOff)
		? Tr(Str::NumberVisibilityOff)
		: tr::lng_edit_privacy_everyone(tr::now);
}

void PutVisibility(
		not_null<Main::Session*> session,
		const QString &visibility,
		Fn<void(bool ok)> done) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			done(false);
			return;
		}
		Api::Put(
			u"/v1/numbers/me/settings"_q,
			{ { u"visibility"_q, visibility } },
			token,
			[=](QJsonObject, int code) {
				const auto ok = (code >= 200 && code < 300);
				if (!ok || visibility != kContacts) {
					done(ok);
					return;
				}
				// "My contacts" is checkable only against a list my own device uploads: ids only.
				auto ids = QJsonArray();
				const auto self = session->userPeerId();
				for (const auto &row : session->data().contactsList()->all()) {
					if (const auto history = row->history()) {
						if (const auto user = history->peer->asUser()) {
							if (user->id != self) {
								ids.push_back(qint64(peerToUser(user->id).bare));
								if (ids.size() >= kMaxContacts) {
									break;
								}
							}
						}
					}
				}
				Api::Put(
					u"/v1/numbers/me/contacts"_q,
					{ { u"contact_tg_ids"_q, ids } },
					token,
					[=](QJsonObject, int code) {
						done(code >= 200 && code < 300);
					});
			});
	});
}

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString current,
		Fn<void(QString)> changed) {
	const auto session = &controller->session();
	box->setTitle(TrValue(Str::NumberHistory));
	box->setWidth(st::boxWideWidth);
	const auto padding = st::boxRowPadding
		+ QMargins(0, st::boxOptionListSkip, 0, 0);

	const auto share = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			Tr(Str::NumberSyncShare),
			ShareEnabled(session),
			st::defaultBoxCheckbox),
		padding);
	share->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		Storage::Set(session, kShareKey, checked);
	}, share->lifetime());

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		TrValue(Str::NumberVisibility),
		st::boxLabel), padding + QMargins(0, st::boxOptionListSkip, 0, 0));
	const auto values = std::vector<QString>{
		kEveryone,
		kContacts,
		kNobody,
		kOff,
	};
	const auto indexOf = [=](const QString &value) {
		const auto i = ranges::find(values, value);
		return (i == end(values)) ? 0 : int(i - begin(values));
	};
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		indexOf(current));
	for (auto i = 0; i != int(values.size()); ++i) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				i,
				VisibilityLabel(values[i]),
				st::defaultBoxCheckbox),
			padding);
	}
	const auto applied = std::make_shared<QString>(current);
	group->setChangedCallback([=](int index) {
		const auto value = values[index];
		PutVisibility(session, value, [=](bool ok) {
			if (ok) {
				*applied = value;
				changed(value);
			} else {
				group->setValue(indexOf(*applied));
				controller->showToast(Tr(Str::AvatarSettingsFailed));
			}
		});
	});
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::NumberSyncShareInfo),
			st::boxDividerLabel),
		padding);

	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
}

} // namespace

void SyncProfile(not_null<UserData*> user) {
	if (user->isBot() || user->isServiceUser()) {
		return;
	}
	const auto userId = peerToUser(user->id).bare;
	const auto now = crl::now();
	auto &synced = LastSynced();
	const auto i = synced.find(userId);
	if (i != end(synced) && now - i->second < kResyncAfter) {
		return;
	}
	synced[userId] = now;
	const auto session = &user->session();
	if (ShareEnabled(session)) {
		Contribute(session, userId);
	}
	Fetch(session, userId);
}

void BuildSettings(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto session = builder.session();
	const auto visibility = std::make_shared<rpl::variable<QString>>(kEveryone);

	// The server holds the choice: it governs what other people's devices may read.
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(u"/v1/numbers/me/settings"_q, token, [=](
				QJsonObject result,
				int code) {
			if (code >= 200 && code < 300) {
				*visibility = result.value(u"visibility"_q).toString(kEveryone);
			}
		});
	});

	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"privacy/svipe_number_history"_q,
		.title = TrValue(Str::NumberHistory),
		.keywords = { u"phone"_q, u"number"_q, u"history"_q },
	});
	builder.addButton({
		.id = u"privacy/svipe_number_visibility"_q,
		.title = TrValue(Str::NumberVisibility),
		.st = &st::settingsButtonNoIcon,
		.label = visibility->value() | rpl::map(VisibilityLabel),
		.onClick = [=] {
			controller->show(Box(
				SettingsBox,
				controller,
				visibility->current(),
				[=](QString value) { *visibility = value; }));
		},
		.keywords = { u"phone"_q, u"number"_q },
	});
	builder.addSkip();
	builder.addDivider();
}

} // namespace Svipe::NumberSync
