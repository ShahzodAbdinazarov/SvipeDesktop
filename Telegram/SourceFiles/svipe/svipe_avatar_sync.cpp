/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_avatar_sync.h"

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
#include "svipe/svipe_avatar_archive.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>

namespace Svipe::AvatarSync {
namespace {

// Android: SvipeAvatarSync constants.
constexpr auto kMinReportInterval = 6 * 60 * 60 * crl::time(1000);
constexpr auto kMaxUploadsPerVisit = 3;
constexpr auto kMaxDownloadsPerVisit = 5;
constexpr auto kMaxUploadBytes = 5 * 1024 * 1024;
constexpr auto kMaxContacts = 5000;

const auto kEnabledKey = u"svipe_avatar_sync_enabled"_q;

const auto kEveryone = u"everyone"_q;
const auto kContacts = u"contacts"_q;
const auto kNobody = u"nobody"_q;
const auto kOff = u"off"_q;

struct State {
	base::flat_map<UserId, QString> lastSignature;
	base::flat_map<UserId, crl::time> lastReportAt;
	base::flat_set<PhotoId> attempted;
	base::flat_set<not_null<Main::Session*>> serverDisabled;
};

State &GlobalState() {
	static auto result = State();
	return result;
}

[[nodiscard]] bool SyncEnabled(not_null<Main::Session*> session) {
	const auto value = Storage::Get(session, kEnabledKey);
	return value.isUndefined() || value.toBool();
}

// Order-independent, so an unchanged profile is silent until something really moves.
[[nodiscard]] QString Signature(
		const std::vector<PhotoId> &live,
		const std::vector<AvatarArchive::Photo> &gone) {
	auto liveSum = uint64(), liveXor = uint64();
	for (const auto id : live) {
		liveSum += id;
		liveXor ^= id;
	}
	auto goneSum = uint64(), goneXor = uint64();
	for (const auto &photo : gone) {
		goneSum += photo.id;
		goneXor ^= photo.id;
	}
	return u"%1:%2:%3|%4:%5:%6"_q
		.arg(live.size()).arg(liveSum).arg(liveXor)
		.arg(gone.size()).arg(goneSum).arg(goneXor);
}

[[nodiscard]] QString Sha256(const QByteArray &bytes) {
	return QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void UploadNext(
		not_null<Main::Session*> session,
		UserId subject,
		std::shared_ptr<std::vector<PhotoId>> ids,
		int index,
		QString token) {
	if (index >= int(ids->size())) {
		return;
	}
	const auto photoId = (*ids)[index];
	const auto next = [=] {
		UploadNext(session, subject, ids, index + 1, token);
	};
	auto file = QFile(AvatarArchive::FilePath(subject, photoId));
	if (!file.open(QIODevice::ReadOnly)) {
		next();
		return;
	}
	const auto bytes = file.readAll();
	if (bytes.isEmpty() || bytes.size() > kMaxUploadBytes) {
		next();
		return;
	}
	GlobalState().attempted.emplace(photoId);
	const auto sha = Sha256(bytes);
	const auto body = QJsonObject{
		{ u"subject_tg_id"_q, qint64(subject.bare) },
		{ u"photo_id"_q, qint64(photoId) },
		{ u"bytes"_q, qint64(bytes.size()) },
		{ u"sha256"_q, sha },
	};
	Api::Post(u"/v1/avatars/upload-url"_q, body, token, [=](
			QJsonObject result,
			int code) {
		if (code < 200 || code >= 300) {
			return; // 403/429: the subject opted out or a quota hit — stop here.
		}
		if (result.value(u"already_stored"_q).toBool()) {
			next();
			return;
		}
		const auto url = result.value(u"url"_q).toString();
		if (url.isEmpty()) {
			return;
		}
		Api::PutBytes(url, bytes, u"image/jpeg"_q, [=](int putCode) {
			if (putCode < 200 || putCode >= 300) {
				return;
			}
			const auto commit = QJsonObject{
				{ u"subject_tg_id"_q, qint64(subject.bare) },
				{ u"photo_id"_q, qint64(photoId) },
				{ u"sha256"_q, sha },
			};
			Api::Post(u"/v1/avatars/commit"_q, commit, token, [=](
					QJsonObject,
					int) {
				next();
			});
		});
	});
}

void DownloadNext(
		UserId subject,
		std::shared_ptr<std::vector<std::tuple<PhotoId, TimeId, QString>>> list,
		int index) {
	if (index >= int(list->size())) {
		return;
	}
	const auto [photoId, date, url] = (*list)[index];
	Api::GetBytes(url, [=](QByteArray bytes, int code) {
		if (code >= 200 && code < 300 && !bytes.isEmpty()) {
			auto file = QFile(AvatarArchive::FilePath(subject, photoId));
			if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				file.write(bytes);
				file.close();
				// Same ledger the local capture writes, so the tab needs no notion of "remote".
				AvatarArchive::RecordSynced(subject, photoId, date);
			}
		}
		DownloadNext(subject, list, index + 1);
	});
}

// The person's archived photos from the shared pool. The proof is a photo id our own Telegram client
// shows for them right now — only someone Telegram lets see this person could have it.
void FetchArchive(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		PhotoId proof,
		const QString &token) {
	const auto subject = peerToUser(user->id);
	const auto self = user->isSelf();
	if (!proof && !self) {
		return;
	}
	const auto path = u"/v1/avatars/%1"_q.arg(subject.bare)
		+ (proof ? u"?proof_photo_id=%1"_q.arg(qint64(proof)) : QString());
	Api::Get(path, token, [=](QJsonObject result, int code) {
		if (code < 200 || code >= 300) {
			return; // 403: Telegram would not show us this person — nothing to do.
		}
		auto list = std::make_shared<
			std::vector<std::tuple<PhotoId, TimeId, QString>>>();
		for (const auto &value : result.value(u"photos"_q).toArray()) {
			const auto object = value.toObject();
			const auto id = PhotoId(object.value(u"photo_id"_q).toInteger());
			const auto url = object.value(u"url"_q).toString();
			if (!id || url.isEmpty() || AvatarArchive::HasFile(subject, id)) {
				continue;
			}
			list->emplace_back(
				id,
				TimeId(object.value(u"photo_date"_q).toInteger()),
				url);
			if (int(list->size()) >= kMaxDownloadsPerVisit) {
				break;
			}
		}
		DownloadNext(subject, list, 0);
	});
}

void Report(not_null<UserData*> user) {
	const auto session = &user->session();
	auto &state = GlobalState();
	if (state.serverDisabled.contains(session) || !SyncEnabled(session)) {
		return;
	}
	const auto subject = peerToUser(user->id);
	const auto live = AvatarArchive::Live(user);
	const auto gone = AvatarArchive::Deleted(user); // empty unless the live list is complete
	if (live.ids.empty() && gone.empty()) {
		return;
	}
	const auto signature = Signature(live.ids, gone);
	const auto now = crl::now();
	const auto i = state.lastSignature.find(subject);
	const auto j = state.lastReportAt.find(subject);
	if (i != end(state.lastSignature)
		&& i->second == signature
		&& j != end(state.lastReportAt)
		&& now - j->second < kMinReportInterval) {
		return;
	}
	state.lastSignature[subject] = signature;
	state.lastReportAt[subject] = now;

	auto liveIds = QJsonArray();
	for (const auto id : live.ids) {
		liveIds.push_back(qint64(id));
	}
	auto deleted = QJsonArray();
	for (const auto &photo : gone) {
		auto object = QJsonObject{ { u"photo_id"_q, qint64(photo.id) } };
		if (photo.date) {
			object.insert(u"photo_date"_q, qint64(photo.date));
		}
		deleted.push_back(object);
	}
	const auto body = QJsonObject{
		{ u"subject_tg_id"_q, qint64(subject.bare) },
		{ u"live_photo_ids"_q, liveIds },
		{ u"deleted"_q, deleted },
	};
	const auto proof = live.ids.empty() ? PhotoId() : live.ids.front();
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Post(u"/v1/avatars/observed"_q, body, token, [=](
				QJsonObject result,
				int code) {
			auto &state = GlobalState();
			if (code < 200 || code >= 300) {
				// Let the next profile view try again rather than burning a report window.
				state.lastSignature.remove(subject);
				return;
			}
			if (!result.value(u"enabled"_q).toBool(true)) {
				state.serverDisabled.emplace(session);
				return;
			}
			// After the report: the live ids just sent are what make our proof verifiable.
			FetchArchive(session, user, proof, token);
			if (!result.value(u"upload_enabled"_q).toBool()) {
				return;
			}
			auto ids = std::make_shared<std::vector<PhotoId>>();
			for (const auto &value : result.value(u"missing"_q).toArray()) {
				const auto id = PhotoId(value.toInteger());
				if (id
					&& !state.attempted.contains(id)
					&& AvatarArchive::HasFile(subject, id)) {
					ids->push_back(id);
					if (int(ids->size()) >= kMaxUploadsPerVisit) {
						break;
					}
				}
			}
			UploadNext(session, subject, ids, 0, token);
		});
	});
}

[[nodiscard]] QString VisibilityLabel(const QString &visibility) {
	return Tr((visibility == kContacts)
		? Str::AvatarVisibilityContacts
		: (visibility == kNobody)
		? Str::AvatarVisibilityNobody
		: (visibility == kOff)
		? Str::AvatarVisibilityOff
		: Str::AvatarVisibilityEveryone);
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
			u"/v1/avatars/me/settings"_q,
			{ { u"visibility"_q, visibility } },
			token,
			[=](QJsonObject, int code) {
				const auto ok = (code >= 200 && code < 300);
				if (!ok || visibility != kContacts) {
					done(ok);
					return;
				}
				// "My contacts" needs the server to know who they are (SvipeAvatarSync.uploadMyContacts).
				auto ids = QJsonArray();
				for (const auto &row : session->data().contactsList()->all()) {
					if (const auto history = row->history()) {
						if (const auto user = history->peer->asUser()) {
							ids.push_back(qint64(peerToUser(user->id).bare));
							if (ids.size() >= kMaxContacts) {
								break;
							}
						}
					}
				}
				Api::Put(
					u"/v1/avatars/me/contacts"_q,
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
	box->setTitle(TrValue(Str::AvatarArchive));
	box->setWidth(st::boxWideWidth);

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
	const auto padding = st::boxRowPadding
		+ QMargins(0, st::boxOptionListSkip, 0, 0);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		TrValue(Str::AvatarVisibilityHeader),
		st::boxLabel));
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
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::AvatarVisibilityInfo),
			st::boxDividerLabel),
		padding);
	const auto applied = std::make_shared<QString>(current);
	group->setChangedCallback([=](int index) {
		const auto value = values[index];
		const auto apply = [=] {
			PutVisibility(session, value, [=](bool ok) {
				if (ok) {
					*applied = value;
					changed(value);
				} else {
					group->setValue(indexOf(*applied));
					controller->showToast(Tr(Str::AvatarSettingsFailed));
				}
			});
		};
		const auto confirm = (value == kContacts)
			? Str::AvatarContactsConfirm
			: (value == kOff)
			? Str::AvatarOptOutConfirm
			: std::optional<Str>();
		if (!confirm) {
			apply();
			return;
		}
		controller->show(Ui::MakeConfirmBox({
			.text = Tr(*confirm),
			.confirmed = [=](Fn<void()> close) {
				apply();
				close();
			},
			.cancelled = [=](Fn<void()> close) {
				group->setValue(indexOf(*applied));
				close();
			},
		}));
	});

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		TrValue(Str::AvatarSyncHeader),
		st::boxLabel), padding + QMargins(0, st::boxOptionListSkip, 0, 0));
	const auto share = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			Tr(Str::AvatarSyncEnabled),
			SyncEnabled(session),
			st::defaultBoxCheckbox),
		padding);
	share->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		Storage::Set(session, kEnabledKey, checked);
	}, share->lifetime());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::AvatarSyncInfo),
			st::boxDividerLabel),
		padding);

	const auto erase = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			Tr(Str::AvatarDeleteArchive),
			st::boxLinkButton),
		padding + QMargins(0, st::boxOptionListSkip, 0, 0));
	erase->setClickedCallback([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = Tr(Str::AvatarDeleteArchiveConfirm),
			.confirmed = [=](Fn<void()> close) {
				close();
				Auth::EnsureToken(session, [=](QString token) {
					if (token.isEmpty()) {
						controller->showToast(Tr(Str::AvatarSettingsFailed));
						return;
					}
					Api::Delete(u"/v1/avatars/me"_q, token, [=](
							QJsonObject,
							int code) {
						controller->showToast(Tr((code >= 200 && code < 300)
							? Str::AvatarArchiveDeleted
							: Str::AvatarSettingsFailed));
					});
				});
			},
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::AvatarDeleteArchiveInfo),
			st::boxDividerLabel),
		padding);

	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
}

} // namespace

void OnProfileSeen(not_null<UserData*> user) {
	if (user->isBot() || user->isServiceUser()) {
		return;
	}
	Report(user);
}

void BuildSettings(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto session = builder.session();
	const auto visibility = std::make_shared<rpl::variable<QString>>(kEveryone);

	// The setting lives server-side: it governs requests from other people's devices.
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(u"/v1/avatars/me/settings"_q, token, [=](
				QJsonObject result,
				int code) {
			if (code >= 200 && code < 300) {
				*visibility = result.value(u"visibility"_q).toString(kEveryone);
			}
		});
	});

	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"privacy/svipe_avatar_archive"_q,
		.title = TrValue(Str::AvatarArchive),
		.keywords = { u"profile"_q, u"photo"_q, u"avatar"_q, u"archive"_q },
	});
	builder.addButton({
		.id = u"privacy/svipe_avatar_visibility"_q,
		.title = TrValue(Str::AvatarVisibilityHeader),
		.st = &st::settingsButtonNoIcon,
		.label = visibility->value() | rpl::map(VisibilityLabel),
		.onClick = [=] {
			controller->show(Box(
				SettingsBox,
				controller,
				visibility->current(),
				[=](QString value) { *visibility = value; }));
		},
		.keywords = { u"profile"_q, u"photo"_q, u"archive"_q },
	});
	builder.addSkip();
	builder.addDivider();
}

} // namespace Svipe::AvatarSync
