/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_message_sync.h"

#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_message_archive.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QUrlQuery>

namespace Svipe::MessageSync {
namespace {

// Android: SvipeMessageSync.MAX_UPLOADS_PER_VISIT — a backlog drains over several visits.
constexpr auto kMaxUploadsPerVisit = 10;
constexpr auto kFetchCooldown = 30 * crl::time(1000);
constexpr auto kOneMonthMs = 30LL * 24 * 60 * 60 * 1000;
constexpr auto kPromptDebounce = crl::time(1500);

const auto kModeKey = u"svipe_msgsync_mode"_q;
const auto kBigShownKey = u"svipe_msgsync_big_shown"_q;
const auto kNextBigAtKey = u"svipe_msgsync_next_big_at"_q;
const auto kMutedUntilKey = u"svipe_msgsync_muted_until"_q;
const auto kSnackDayPrefix = u"svipe_msgsync_snack_day_"_q;

struct Pending {
	QString key;
	int64 author = 0;
	TimeId date = 0;
	TimeId editDate = 0;
	int kind = 0;
	bool hasMedia = false;
	QByteArray message;
};

struct SessionState {
	rpl::event_stream<QString> modeChanges;
	base::flat_set<PeerId> syncing;
	base::flat_map<PeerId, crl::time> lastFetch;
	crl::time lastPrompt = 0;
	bool serverDisabled = false;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<SessionState>> States;

[[nodiscard]] SessionState &StateFor(not_null<Main::Session*> session) {
	const auto i = States.find(session);
	if (i != end(States)) {
		return *i->second;
	}
	session->lifetime().add([=] {
		States.remove(session);
	});
	return *States.emplace(
		session,
		std::make_unique<SessionState>()).first->second;
}

[[nodiscard]] QString Sha256Hex(const QByteArray &data) {
	return QString::fromLatin1(
		QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// MessageObject.getPeerId on Android: users positive, chats and channels negative.
[[nodiscard]] int64 DialogId(PeerId peer) {
	if (peerIsUser(peer)) {
		return int64(peerToUser(peer).bare);
	} else if (peerIsChat(peer)) {
		return -int64(peerToChat(peer).bare);
	}
	return -int64(peerToChannel(peer).bare);
}

[[nodiscard]] int64 MediaId(const MTPDmessage &data) {
	const auto media = data.vmedia();
	if (!media) {
		return 0;
	}
	return media->match([](const MTPDmessageMediaPhoto &media) {
		const auto photo = media.vphoto();
		return (photo && photo->type() == mtpc_photo)
			? int64(photo->c_photo().vid().v)
			: int64(0);
	}, [](const MTPDmessageMediaDocument &media) {
		const auto document = media.vdocument();
		return (document && document->type() == mtpc_document)
			? int64(document->c_document().vid().v)
			: int64(0);
	}, [](const auto &) {
		return int64(0);
	});
}

[[nodiscard]] crl::time NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] QString Mode(not_null<Main::Session*> session) {
	return Storage::Get(session, kModeKey).toString();
}

[[nodiscard]] bool Granted(const QString &mode) {
	return (mode == kWithPartner) || (mode == kSelfOnly);
}

void StoreMode(not_null<Main::Session*> session, const QString &mode) {
	if (Mode(session) == mode) {
		return;
	}
	Storage::Set(session, kModeKey, mode);
	StateFor(session).modeChanges.fire_copy(mode);
}

[[nodiscard]] rpl::producer<QString> ModeValue(
		not_null<Main::Session*> session) {
	return rpl::single(
		Mode(session)
	) | rpl::then(StateFor(session).modeChanges.events());
}

[[nodiscard]] QString ModeLabel(const QString &mode) {
	return Tr((mode == kWithPartner)
		? Str::MsgSyncWithPartner
		: (mode == kSelfOnly)
		? Str::MsgSyncSelfOnly
		: (mode == kOff)
		? Str::MsgSyncOff
		: Str::MsgSyncNotSet);
}

[[nodiscard]] bool IsSyncablePeer(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user
		&& !user->isBot()
		&& !user->isSelf()
		&& !user->isServiceUser()
		&& (user->id != session->userPeerId());
}

void PutMode(
		not_null<Main::Session*> session,
		const QString &mode,
		Fn<void(bool ok, int deleted)> done) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			if (done) done(false, 0);
			return;
		}
		Api::Put(u"/v1/msg-sync/me/mode"_q, { { u"mode"_q, mode } }, token, [=](
				QJsonObject result,
				int code) {
			if (code == 503) {
				StateFor(session).serverDisabled = true;
			}
			const auto ok = (code >= 200 && code < 300);
			if (done) done(ok, ok ? result.value(u"deleted"_q).toInt() : 0);
		});
	});
}

void Fetch(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer);

void Commit(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QString &mode,
		const QString &token,
		std::shared_ptr<std::vector<Pending>> queue,
		int index) {
	auto &state = StateFor(session);
	if (index >= int(queue->size())) {
		state.syncing.remove(peer->id);
		return;
	}
	const auto &item = (*queue)[index];
	const auto body = QJsonObject{
		{ u"mode"_q, mode },
		{ u"peer_tg_id"_q, double(DialogId(peer->id)) },
		{ u"key"_q, item.key },
		{ u"ciphertext_b64"_q, QString::fromLatin1(item.message.toBase64()) },
	};
	const auto key = item.key;
	const auto message = item.message;
	Api::Post(u"/v1/msg-sync/commit"_q, body, token, [=](
			QJsonObject result,
			int code) {
		if (code >= 200 && code < 300) {
			MessageArchive::SetMergeKey(session, peer->id, message, key);
		}
		Commit(session, peer, mode, token, queue, index + 1);
	});
}

void Observe(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QString &mode,
		std::vector<Pending> staged) {
	Auth::EnsureToken(session, [=, staged = std::move(staged)](QString token) {
		auto &state = StateFor(session);
		if (token.isEmpty()) {
			state.syncing.remove(peer->id);
			return;
		}
		auto items = QJsonArray();
		for (const auto &item : staged) {
			items.push_back(QJsonObject{
				{ u"key"_q, item.key },
				{ u"author_tg_id"_q, double(item.author) },
				{ u"date"_q, item.date },
				{ u"edit_date"_q, item.editDate },
				{ u"kind"_q, item.kind },
				{ u"has_media"_q, item.hasMedia },
			});
		}
		const auto body = QJsonObject{
			{ u"mode"_q, mode },
			{ u"peer_tg_id"_q, double(DialogId(peer->id)) },
			{ u"items"_q, items },
		};
		Api::Post(u"/v1/msg-sync/observed"_q, body, token, [=](
				QJsonObject result,
				int code) {
			auto &state = StateFor(session);
			if (code == 503) {
				state.serverDisabled = true;
			}
			if (code < 200
				|| code >= 300
				|| !result.value(u"enabled"_q).toBool()) {
				state.syncing.remove(peer->id);
				return;
			}
			auto byKey = base::flat_map<QString, Pending>();
			for (const auto &item : staged) {
				byKey.emplace(item.key, item);
			}
			auto queue = std::make_shared<std::vector<Pending>>();
			for (const auto &value : result.value(u"missing"_q).toArray()) {
				const auto i = byKey.find(value.toString());
				if (i != end(byKey)) {
					queue->push_back(i->second);
					if (queue->size() >= kMaxUploadsPerVisit) {
						break;
					}
				}
			}
			Commit(session, peer, mode, token, queue, 0);
		});
	});
}

void Upload(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QString &mode) {
	auto &state = StateFor(session);
	if (state.syncing.contains(peer->id)) {
		return;
	}
	const auto self = DialogId(session->userPeerId());
	const auto partner = DialogId(peer->id);
	auto staged = std::vector<Pending>();
	auto seen = base::flat_set<QString>();
	for (const auto &entry : MessageArchive::Load(session, peer->id)) {
		const auto parsed = MessageArchive::Parse(entry.message);
		if (!parsed) {
			continue;
		}
		const auto &data = parsed->c_message();
		const auto author = data.vfrom_id()
			? DialogId(peerFromMTP(*data.vfrom_id()))
			: int64(0);
		// My archive of the chat is mine whoever wrote the message; the mode only decides who else
		// may read it (the server's partner_may_read).
		if (!author || (author != self && author != partner)) {
			continue;
		}
		const auto mediaId = MediaId(data);
		const auto key = MergeKey(
			author,
			data.vdate().v,
			qs(data.vmessage()),
			mediaId);
		if (!seen.emplace(key).second) {
			continue;
		}
		staged.push_back({
			.key = key,
			.author = author,
			.date = data.vdate().v,
			.editDate = data.vedit_date().value_or_empty(),
			.kind = int(entry.kind),
			.hasMedia = (mediaId != 0),
			.message = entry.message,
		});
	}
	if (staged.empty()) {
		return;
	}
	state.syncing.emplace(peer->id);
	Observe(session, peer, mode, std::move(staged));
}

void Fetch(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer) {
	auto &state = StateFor(session);
	const auto now = crl::now();
	const auto i = state.lastFetch.find(peer->id);
	if (i != end(state.lastFetch) && now - i->second < kFetchCooldown) {
		return;
	}
	state.lastFetch[peer->id] = now;
	// My own archive of the chat (from my other devices) plus the partner's when they share it.
	const auto path = u"/v1/msg-sync/chat?peer_tg_id="_q
		+ QString::number(DialogId(peer->id));
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(path, token, [=](QJsonObject result, int code) {
			if (code == 503) {
				StateFor(session).serverDisabled = true;
			}
			if (code < 200 || code >= 300) {
				return;
			}
			// What this device already holds, by the same key, so a deletion it captured itself
			// and then uploaded is not added a second time.
			auto local = base::flat_set<QString>();
			for (const auto &entry : MessageArchive::Load(session, peer->id)) {
				if (!entry.mergeKey.isEmpty()) {
					local.emplace(entry.mergeKey);
				} else if (const auto parsed = MessageArchive::Parse(
						entry.message)) {
					const auto &data = parsed->c_message();
					if (const auto from = data.vfrom_id()) {
						local.emplace(MergeKey(
							DialogId(peerFromMTP(*from)),
							data.vdate().v,
							qs(data.vmessage()),
							MediaId(data)));
					}
				}
			}
			auto synced = std::vector<MessageArchive::Entry>();
			for (const auto &value : result.value(u"items"_q).toArray()) {
				const auto item = value.toObject();
				const auto key = item.value(u"merge_key"_q).toString();
				const auto message = QByteArray::fromBase64(
					item.value(u"message_b64"_q).toString().toLatin1());
				if (key.isEmpty()
					|| local.contains(key)
					|| !MessageArchive::Parse(message)) {
					continue;
				}
				synced.push_back({
					.kind = MessageArchive::Kind(
						item.value(u"kind"_q).toInt()),
					.date = TimeId(item.value(u"date"_q).toInt()),
					.editDate = TimeId(item.value(u"edit_date"_q).toInt()),
					.archivedAt = 0,
					.id = MsgId(0),
					.mergeKey = key,
					.message = message,
				});
			}
			if (!synced.empty()) {
				MessageArchive::AddSynced(
					session,
					peer->id,
					std::move(synced));
			}
		});
	});
}

void ApplyMode(
		not_null<Main::Session*> session,
		const QString &mode,
		bool fromReAsk,
		PeerData *peer) {
	StoreMode(session, mode);
	if (mode == kOff) {
		if (!fromReAsk) {
			// First rejection: the big dialog reappears once, a month from now.
			Storage::Set(session, kNextBigAtKey, double(NowMs() + kOneMonthMs));
		}
	} else {
		Storage::Set(session, kNextBigAtKey, 0.);
	}
	PutMode(session, mode, nullptr);
	if (Granted(mode) && peer) {
		Upload(session, peer, mode);
		Fetch(session, peer);
	}
}

void ShowGranted(not_null<Window::SessionController*> controller) {
	controller->showToast(Tr(Str::MsgSyncGranted));
}

void ShowBigDialog(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto session = &controller->session();
	const auto fromReAsk = Storage::Get(session, kNextBigAtKey).toDouble() > 0;
	Storage::Set(session, kBigShownKey, true);
	Storage::Set(session, kNextBigAtKey, 0.);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TrValue(Str::MsgSyncPromptTitle));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(Ui::Text::RichLangValue(
				Tr(Str::MsgSyncPromptMessage))),
			st::boxLabel));
		const auto choose = [=](const QString &mode) {
			ApplyMode(session, mode, fromReAsk, peer);
			box->closeBox();
			if (Granted(mode)) {
				ShowGranted(controller);
			}
		};
		box->addButton(TrValue(Str::MsgSyncAllow), [=] {
			choose(kWithPartner);
		});
		box->addButton(TrValue(Str::MsgSyncForMe), [=] {
			choose(kSelfOnly);
		});
		box->addLeftButton(TrValue(Str::MsgSyncDecline), [=] {
			choose(kOff);
		});
	}));
}

void ShowSnackbar(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		int64 today) {
	const auto session = &controller->session();
	Storage::Set(
		session,
		kSnackDayPrefix + QString::number(peer->id.value),
		double(today));
	auto text = TextWithEntities{ Tr(Str::MsgSyncSnackbar) + ' ' };
	text.append(Ui::Text::Link(Tr(Str::MsgSyncAllow)));
	controller->showToast(Ui::Toast::Config{
		.text = std::move(text),
		.filter = [=](const ClickHandlerPtr &, Qt::MouseButton) {
			ApplyMode(session, kWithPartner, false, peer);
			ShowGranted(controller);
			return false;
		},
		.duration = 6 * crl::time(1000),
	});
}

// SvipeMsgSyncPrompt.decide, step for step.
void MaybePrompt(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto session = &controller->session();
	auto &state = StateFor(session);
	if (state.serverDisabled || !IsSyncablePeer(session, peer)) {
		return;
	}
	const auto now = crl::now();
	if (now - state.lastPrompt < kPromptDebounce) {
		return;
	}
	state.lastPrompt = now;

	const auto mode = Mode(session);
	if (Granted(mode)) {
		return;
	}
	const auto nowMs = NowMs();
	if (Storage::Get(session, kMutedUntilKey).toDouble() > nowMs) {
		return;
	}
	const auto nextBigAt = Storage::Get(session, kNextBigAtKey).toDouble();
	if (!Storage::Get(session, kBigShownKey).toBool()
		|| (nextBigAt > 0 && nowMs >= nextBigAt)) {
		ShowBigDialog(controller, peer);
		return;
	}
	const auto today = int64(nowMs / (24LL * 60 * 60 * 1000));
	const auto shownToday = int64(Storage::Get(
		session,
		kSnackDayPrefix + QString::number(peer->id.value)).toDouble());
	if (shownToday != today) {
		ShowSnackbar(controller, peer, today);
	}
}

void PromptIfActive(not_null<Main::Session*> session, PeerId peerId) {
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller || &controller->session() != session) {
		return;
	}
	const auto peer = session->data().peerLoaded(peerId);
	const auto active = controller->activeChatCurrent().peer();
	if (peer && active == peer) {
		MaybePrompt(controller, peer);
	}
}

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	box->setTitle(TrValue(Str::MsgSyncTitle));
	box->setWidth(st::boxWideWidth);

	const auto modes = std::vector<QString>{ kWithPartner, kSelfOnly, kOff };
	const auto indexOf = [=](const QString &mode) {
		const auto i = ranges::find(modes, mode);
		return (i == end(modes)) ? -1 : int(i - begin(modes));
	};
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		TrValue(Str::MsgSyncModeHeader),
		st::boxLabel));
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		indexOf(Mode(session)));
	const auto labels = std::vector<Str>{
		Str::MsgSyncWithPartner,
		Str::MsgSyncSelfOnly,
		Str::MsgSyncOff,
	};
	for (auto i = 0; i != int(modes.size()); ++i) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				i,
				Tr(labels[i]),
				st::defaultBoxCheckbox),
			st::boxRowPadding + QMargins(0, st::boxOptionListSkip, 0, 0));
	}
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::MsgSyncModeInfo),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxOptionListSkip, 0, 0));
	group->setChangedCallback([=](int value) {
		const auto mode = modes[value];
		const auto apply = [=] {
			ApplyMode(session, mode, true, nullptr);
		};
		if (mode == kOff && Granted(Mode(session))) {
			controller->show(Ui::MakeConfirmBox({
				.text = Tr(Str::MsgSyncOffConfirm),
				.confirmed = [=](Fn<void()> close) {
					apply();
					close();
				},
				.cancelled = [=](Fn<void()> close) {
					group->setValue(indexOf(Mode(session)));
					close();
				},
			}));
		} else {
			apply();
		}
	});

	const auto muted = Storage::Get(session, kMutedUntilKey).toDouble()
		> NowMs();
	const auto remind = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			Tr(Str::MsgSyncRemind),
			muted,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxOptionListSkip * 2, 0, 0));
	remind->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		Storage::Set(
			session,
			kMutedUntilKey,
			checked ? double(NowMs() + kOneMonthMs) : 0.);
	}, remind->lifetime());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::MsgSyncRemindInfo),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxOptionListSkip, 0, 0));

	const auto erase = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			Tr(Str::MsgSyncDelete),
			st::boxLinkButton),
		st::boxRowPadding + QMargins(0, st::boxOptionListSkip * 2, 0, 0));
	erase->setClickedCallback([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = Tr(Str::MsgSyncDeleteConfirm),
			.confirmed = [=](Fn<void()> close) {
				close();
				Auth::EnsureToken(session, [=](QString token) {
					if (token.isEmpty()) {
						controller->showToast(Tr(Str::MsgSyncFailed));
						return;
					}
					Api::Delete(u"/v1/msg-sync/me"_q, token, [=](
							QJsonObject result,
							int code) {
						const auto ok = (code >= 200 && code < 300);
						if (ok) {
							StoreMode(session, kOff);
							group->setValue(indexOf(kOff));
						}
						controller->showToast(Tr(ok
							? Str::MsgSyncArchiveDeleted
							: Str::MsgSyncFailed));
					});
				});
			},
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TrValue(Str::MsgSyncDeleteInfo),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxOptionListSkip, 0, 0));

	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
}

} // namespace

QString MergeKey(
		int64 authorId,
		TimeId date,
		const QString &text,
		int64 mediaId) {
	// SvipeMessageSync.contentHash / mergeKey: the separator is a NUL, so "a" + "b" and "ab" + ""
	// cannot collide.
	const auto content = Sha256Hex(
		(text + QChar(0) + QString::number(mediaId)).toUtf8());
	return Sha256Hex((QString::number(authorId)
		+ ':'
		+ QString::number(date)
		+ ':'
		+ content).toUtf8());
}

void Start(not_null<Main::Session*> session) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(u"/v1/msg-sync/me/mode"_q, token, [=](
				QJsonObject result,
				int code) {
			if (code == 503) {
				StateFor(session).serverDisabled = true;
			} else if (code >= 200 && code < 300) {
				const auto mode = result.value(u"mode"_q);
				if (mode.isString()) {
					StoreMode(session, mode.toString());
				}
			}
		});
	});
	MessageArchive::Updates(
		session
	) | rpl::on_next([=](PeerId peer) {
		PromptIfActive(session, peer);
	}, session->lifetime());
}

void OnChatOpened(not_null<History*> history) {
	const auto session = &history->session();
	const auto peer = history->peer;
	if (StateFor(session).serverDisabled || !IsSyncablePeer(session, peer)) {
		return;
	}
	// Reading does not depend on my mode: the partner's archive reaches me when THEY share it.
	Fetch(session, peer);
	const auto mode = Mode(session);
	if (Granted(mode)) {
		Upload(session, peer, mode);
	}
}

void BuildSettings(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto session = builder.session();

	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"privacy/svipe_msg_sync"_q,
		.title = TrValue(Str::MsgSyncTitle),
		.keywords = { u"deleted"_q, u"edited"_q, u"sync"_q, u"svipe"_q },
	});
	builder.addButton({
		.id = u"privacy/svipe_msg_sync_mode"_q,
		.title = TrValue(Str::MsgSyncModeHeader),
		.st = &st::settingsButtonNoIcon,
		.label = ModeValue(session) | rpl::map(ModeLabel),
		.onClick = [=] {
			controller->show(Box(SettingsBox, controller));
		},
		.keywords = { u"deleted"_q, u"edited"_q, u"sync"_q },
	});
	builder.addSkip();
	builder.addDivider();
}

} // namespace Svipe::MessageSync
