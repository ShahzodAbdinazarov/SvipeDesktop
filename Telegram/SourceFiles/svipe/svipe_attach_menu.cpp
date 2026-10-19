/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_attach_menu.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "inline_bots/inline_bot_result.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_storage.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/checkbox.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUrl>

namespace Svipe::AttachMenu {
namespace {

// The Android client keeps the same lists in SharedPreferences.
const auto kKey = u"svipe_attachmenu"_q;

// The add dialog waits this long for the bot's settings, then shows anyway.
constexpr auto kConfigTimeout = 2 * crl::time(1000);
// The mini app waits this long for Svipe's signature, then loads as it is.
constexpr auto kSignTimeout = crl::time(2500);

struct Global {
	// Sessions (by user id) whose list was refreshed in this run.
	base::flat_set<uint64> refreshed;
	// The settings of every bot asked about in this run, the added ones' are also stored.
	base::flat_map<uint64, QJsonObject> configs;
	base::flat_set<uint64> resolving;
	rpl::event_stream<> changes;
};

Global &G() {
	static auto result = Global();
	return result;
}

// { "bots": [id], "declined": [id], "usernames": {id: username}, "configs": {id: config} }.
[[nodiscard]] QJsonObject State(not_null<Main::Session*> session) {
	return Storage::Get(session, kKey).toObject();
}

void SaveState(not_null<Main::Session*> session, const QJsonObject &state) {
	Storage::Set(session, kKey, state);
}

[[nodiscard]] std::vector<uint64> Ids(
		const QJsonObject &state,
		const QString &list) {
	auto result = std::vector<uint64>();
	for (const auto &value : state.value(list).toArray()) {
		result.push_back(uint64(value.toString().toULongLong()));
	}
	return result;
}

[[nodiscard]] std::vector<uint64> Ids(
		not_null<Main::Session*> session,
		const QString &list) {
	return Ids(State(session), list);
}

[[nodiscard]] QJsonArray Pack(const std::vector<uint64> &ids) {
	auto result = QJsonArray();
	for (const auto id : ids) {
		result.push_back(QString::number(id));
	}
	return result;
}

void Changed() {
	G().changes.fire({});
}

[[nodiscard]] uint64 Id(not_null<UserData*> bot) {
	return peerToUser(bot->id).bare;
}

[[nodiscard]] QJsonObject Config(not_null<Main::Session*> session, uint64 id) {
	const auto i = G().configs.find(id);
	if (i != end(G().configs)) {
		return i->second;
	}
	return State(session).value(u"configs"_q).toObject().value(
		QString::number(id)).toObject();
}

[[nodiscard]] QJsonObject Config(not_null<UserData*> bot) {
	return Config(&bot->session(), Id(bot));
}

// The answer, for the account's other devices.
void PushState(
		not_null<Main::Session*> session,
		uint64 id,
		const QString &state,
		const QString &username) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		auto body = QJsonObject{ { u"state"_q, state } };
		if (!username.isEmpty()) {
			body.insert(u"bot_username"_q, username);
		}
		Api::Put(
			u"/v1/attach/bots/%1"_q.arg(id),
			body,
			token,
			[=](QJsonObject, int code) {
				if (code < 200 || code >= 300) {
					LOG(("Svipe attach menu: push %1 failed, %2.").arg(id).arg(code));
				}
			});
	});
}

void SetState(not_null<UserData*> bot, bool added) {
	const auto session = &bot->session();
	auto state = State(session);
	auto bots = Ids(state, u"bots"_q);
	auto declined = Ids(state, u"declined"_q);
	auto usernames = state.value(u"usernames"_q).toObject();
	auto configs = state.value(u"configs"_q).toObject();
	const auto id = Id(bot);
	const auto key = QString::number(id);
	bots.erase(ranges::remove(bots, id), end(bots));
	declined.erase(ranges::remove(declined, id), end(declined));
	(added ? bots : declined).push_back(id);
	if (added) {
		usernames.insert(key, bot->username());
		configs.insert(key, Config(bot));
	} else {
		usernames.remove(key);
		configs.remove(key);
	}
	state.insert(u"bots"_q, Pack(bots));
	state.insert(u"declined"_q, Pack(declined));
	state.insert(u"usernames"_q, usernames);
	state.insert(u"configs"_q, configs);
	SaveState(session, state);
	PushState(
		session,
		id,
		added ? u"added"_q : u"declined"_q,
		bot->username());
	Changed();
}

// The settings of these bots, then done on the main thread, whatever the answer. The endpoint is
// public: no token needed.
void RefreshConfig(
		not_null<Main::Session*> session,
		std::vector<uint64> ids,
		Fn<void()> done) {
	if (ids.empty()) {
		if (done) {
			done();
		}
		return;
	}
	auto query = QStringList();
	for (const auto id : ids) {
		query.push_back(QString::number(id));
	}
	Api::Get(
		u"/v1/attach/config?ids="_q + query.join(','),
		QString(),
		crl::guard(session, [=](QJsonObject result, int code) {
			const auto map = (code == 200)
				? result.value(u"bots"_q).toObject()
				: QJsonObject();
			if (!map.isEmpty()) {
				auto state = State(session);
				const auto bots = Ids(state, u"bots"_q);
				auto configs = state.value(u"configs"_q).toObject();
				auto changed = false;
				for (auto i = map.begin(); i != map.end(); ++i) {
					const auto id = uint64(i.key().toULongLong());
					if (!id) {
						continue;
					}
					const auto config = i.value().toObject();
					G().configs[id] = config;
					if (ranges::contains(bots, id)
						&& configs.value(i.key()).toObject() != config) {
						configs.insert(i.key(), config);
						changed = true;
					}
				}
				if (changed) {
					state.insert(u"configs"_q, configs);
					SaveState(session, state);
					Changed();
				}
			}
			if (done) {
				done();
			}
		}));
}

// A bot added on another device (or before a restart) may be unknown here: find it by its
// username, once per run.
void ResolveMissing(not_null<Main::Session*> session) {
	const auto state = State(session);
	const auto usernames = state.value(u"usernames"_q).toObject();
	for (const auto id : Ids(state, u"bots"_q)) {
		const auto username = usernames.value(QString::number(id)).toString();
		if (username.isEmpty()
			|| G().resolving.contains(id)
			|| session->data().userLoaded(UserId(id))) {
			continue;
		}
		G().resolving.emplace(id);
		session->api().request(MTPcontacts_ResolveUsername(
			MTP_flags(0),
			MTP_string(username),
			MTP_string()
		)).done(crl::guard(session, [=](
				const MTPcontacts_ResolvedPeer &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			if (peerFromMTP(data.vpeer()) == peerFromUser(UserId(id))) {
				Changed();
			}
		})).send();
	}
}

// The answers given on the account's other devices. A bot added elsewhere is added here without a
// new dialog (the person already agreed there); one removed elsewhere goes. Bots only this device
// knows are sent up.
void SyncFromServer(not_null<Main::Session*> session) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(
			u"/v1/attach/bots"_q,
			token,
			crl::guard(session, [=](QJsonObject result, int code) {
				if (code != 200 || !result.value(u"bots"_q).isArray()) {
					return;
				}
				auto state = State(session);
				auto bots = Ids(state, u"bots"_q);
				auto declined = Ids(state, u"declined"_q);
				auto usernames = state.value(u"usernames"_q).toObject();
				auto configs = state.value(u"configs"_q).toObject();
				auto known = std::vector<uint64>();
				auto fresh = std::vector<uint64>();
				auto changed = false;
				for (const auto &value : result.value(u"bots"_q).toArray()) {
					const auto item = value.toObject();
					const auto id = uint64(
						item.value(u"bot_id"_q).toVariant().toULongLong());
					if (!id) {
						continue;
					}
					known.push_back(id);
					const auto key = QString::number(id);
					if (item.value(u"state"_q).toString() == u"added"_q) {
						if (!ranges::contains(bots, id)) {
							bots.push_back(id);
							declined.erase(
								ranges::remove(declined, id),
								end(declined));
							const auto username = item.value(
								u"bot_username"_q).toString();
							if (!username.isEmpty()) {
								usernames.insert(key, username);
							}
							fresh.push_back(id);
							changed = true;
						}
					} else {
						if (ranges::contains(bots, id)) {
							bots.erase(ranges::remove(bots, id), end(bots));
							usernames.remove(key);
							configs.remove(key);
							changed = true;
						}
						if (!ranges::contains(declined, id)) {
							declined.push_back(id);
							changed = true;
						}
					}
				}
				for (const auto id : bots) {
					if (!ranges::contains(known, id)) {
						PushState(
							session,
							id,
							u"added"_q,
							usernames.value(QString::number(id)).toString());
					}
				}
				if (changed) {
					state.insert(u"bots"_q, Pack(bots));
					state.insert(u"declined"_q, Pack(declined));
					state.insert(u"usernames"_q, usernames);
					state.insert(u"configs"_q, configs);
					SaveState(session, state);
					ResolveMissing(session);
					RefreshConfig(session, fresh, nullptr);
					Changed();
				}
			}));
	});
}

void Refresh(not_null<Main::Session*> session) {
	if (!G().refreshed.emplace(session->userId().bare).second) {
		return;
	}
	crl::on_main(session, [=] {
		RefreshConfig(session, Ids(session, u"bots"_q), nullptr);
		SyncFromServer(session);
		ResolveMissing(session);
	});
}

void ShowBox(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		Fn<void(bool shownNow, bool added)> done) {
	const auto answered = std::make_shared<bool>(false);
	const auto answer = [=](bool added, bool allowWrite) {
		if (std::exchange(*answered, true)) {
			return;
		}
		SetState(bot, added);
		if (added) {
			show->showToast(tr::lng_bot_add_to_menu_done(tr::now));
			if (allowWrite) {
				const auto session = &bot->session();
				session->api().request(MTPbots_AllowSendMessage(
					bot->inputUser()
				)).done([=](const MTPUpdates &result) {
					session->api().applyUpdates(result);
				}).send();
			}
		}
		done(true, added);
	};
	// Telegram's request_write_access: the same checkbox its own dialog shows, on by default.
	const auto writeAccess = Config(bot).value(u"write_access"_q).toBool();
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto allowed = std::make_shared<Ui::Checkbox*>(nullptr);
		Ui::ConfirmBox(box, {
			.text = tr::lng_bot_add_to_menu(
				tr::now,
				lt_bot,
				tr::bold(bot->name()),
				tr::marked),
			.confirmed = [=](Fn<void()> close) {
				answer(true, (*allowed) && (*allowed)->checked());
				close();
			},
			.cancelled = [=](Fn<void()> close) {
				answer(false, false);
				close();
			},
		});
		if (writeAccess) {
			(*allowed) = box->addRow(
				object_ptr<Ui::Checkbox>(
					box,
					tr::lng_url_auth_allow_messages(
						tr::now,
						lt_bot,
						tr::bold(bot->name()),
						tr::marked),
					true,
					st::urlAuthCheckbox),
				style::margins(
					st::boxRowPadding.left(),
					st::boxRowPadding.left(),
					st::boxRowPadding.right(),
					st::boxRowPadding.left()));
			(*allowed)->setAllowTextLines();
		}
		box->boxClosing() | rpl::on_next([=] {
			answer(false, false);
		}, box->lifetime());
	}));
}

// The query string Telegram signed for this launch, parsed: chat_type and chat_instance are taken
// from it, so Svipe's signature can't be carried to another chat.
[[nodiscard]] QJsonObject ContextBody(
		not_null<UserData*> bot,
		not_null<PeerData*> peer,
		const QString &url) {
	auto body = QJsonObject{ { u"bot_id"_q, qint64(Id(bot)) } };
	const auto initData = Auth::ExtractInitData(url);
	for (const auto &pair : initData.split('&')) {
		const auto eq = pair.indexOf('=');
		if (eq <= 0) {
			continue;
		}
		const auto key = pair.mid(0, eq);
		if (key == u"chat_type"_q || key == u"chat_instance"_q) {
			body.insert(key, QUrl::fromPercentEncoding(pair.mid(eq + 1).toUtf8()));
		}
	}
	const auto optional = [](QJsonObject &object, const QString &key, const QString &value) {
		if (!value.isEmpty()) {
			object.insert(key, value);
		}
	};
	if (const auto user = peer->asUser()) {
		auto receiver = QJsonObject{
			{ u"id"_q, qint64(peerToUser(user->id).bare) },
			{ u"first_name"_q, user->firstName },
		};
		optional(receiver, u"last_name"_q, user->lastName);
		optional(receiver, u"username"_q, user->username());
		if (user->isBot()) {
			receiver.insert(u"is_bot"_q, true);
		}
		body.insert(u"receiver"_q, receiver);
	} else {
		// Ids as the Bot API writes them: -100… for channels and supergroups, -… for groups.
		const auto channel = peer->asChannel();
		auto chat = QJsonObject{
			{ u"id"_q, channel
				? -(qint64(1000000000000) + qint64(peerToChannel(peer->id).bare))
				: -qint64(peerToChat(peer->id).bare) },
			{ u"type"_q, !channel
				? u"group"_q
				: channel->isBroadcast()
				? u"channel"_q
				: u"supergroup"_q },
			{ u"title"_q, peer->name() },
		};
		optional(chat, u"username"_q, peer->username());
		body.insert(u"chat"_q, chat);
	}
	return body;
}

// Whether the prepared message may go to this chat: the bot chose the kinds of chat it may.
[[nodiscard]] bool Allows(
		const MTPVector<MTPInlineQueryPeerType> &types,
		not_null<UserData*> bot,
		not_null<PeerData*> peer) {
	if (types.v.isEmpty()) {
		return true;
	}
	for (const auto &type : types.v) {
		const auto allowed = type.match([&](const MTPDinlineQueryPeerTypePM &) {
			return peer->isUser() && !peer->asUser()->isBot();
		}, [&](const MTPDinlineQueryPeerTypeBotPM &) {
			return peer->isUser() && peer->asUser()->isBot() && peer != bot;
		}, [&](const MTPDinlineQueryPeerTypeSameBotPM &) {
			return (peer == bot);
		}, [&](const MTPDinlineQueryPeerTypeChat &) {
			return peer->isChat();
		}, [&](const MTPDinlineQueryPeerTypeMegagroup &) {
			return peer->isMegagroup();
		}, [&](const MTPDinlineQueryPeerTypeBroadcast &) {
			return peer->isBroadcast();
		});
		if (allowed) {
			return true;
		}
	}
	return false;
}

} // namespace

std::vector<not_null<UserData*>> Bots(not_null<Main::Session*> session) {
	Refresh(session);
	auto result = std::vector<not_null<UserData*>>();
	for (const auto id : Ids(session, u"bots"_q)) {
		if (const auto user = session->data().userLoaded(UserId(id))) {
			result.push_back(user);
		}
	}
	return result;
}

std::vector<not_null<UserData*>> SideMenuBots(
		not_null<Main::Session*> session) {
	auto result = Bots(session);
	result.erase(ranges::remove_if(result, [](not_null<UserData*> bot) {
		return !Config(bot).value(u"side_menu"_q).toBool();
	}), end(result));
	return result;
}

bool Contains(not_null<UserData*> bot) {
	return ranges::contains(Ids(&bot->session(), u"bots"_q), Id(bot));
}

InlineBots::PeerTypes Types(not_null<UserData*> bot) {
	using InlineBots::PeerType;
	const auto names = Config(bot).value(u"peer_types"_q).toArray();
	if (names.isEmpty()) {
		return InlineBots::PeerTypes(PeerType::Bot)
			| PeerType::User
			| PeerType::Group
			| PeerType::Broadcast;
	}
	auto result = InlineBots::PeerTypes();
	for (const auto &name : names) {
		const auto type = name.toString();
		if (type == u"same_bot"_q) {
			result |= PeerType::SameBot;
		} else if (type == u"bots"_q) {
			result |= PeerType::Bot;
		} else if (type == u"users"_q) {
			result |= PeerType::User;
		} else if (type == u"groups"_q) {
			result |= PeerType::Group;
		} else if (type == u"channels"_q) {
			result |= PeerType::Broadcast;
		}
	}
	return result;
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
	// The bot's settings decide whether the dialog asks to let it write to the user; a slow answer
	// holds the dialog up for two seconds at most.
	const auto session = &bot->session();
	const auto shown = std::make_shared<bool>(false);
	const auto showBox = [=] {
		if (std::exchange(*shown, true)) {
			return;
		} else if (!show->valid()) {
			done(false, false);
			return;
		}
		ShowBox(show, bot, done);
	};
	RefreshConfig(session, { Id(bot) }, showBox);
	base::call_delayed(kConfigTimeout, session, showBox);
}

void Remove(std::shared_ptr<Ui::Show> show, not_null<UserData*> bot) {
	if (!Contains(bot)) {
		return;
	}
	SetState(bot, false);
	show->showToast(tr::lng_bot_remove_from_menu_done(tr::now));
}

rpl::producer<> Changes() {
	return G().changes.events();
}

void Open(
		std::shared_ptr<Ui::Show> show,
		base::weak_ptr<Window::SessionController> controller,
		not_null<UserData*> bot,
		std::optional<::Api::SendAction> chat,
		const QString &startCommand) {
	const auto info = bot->botInfo.get();
	const auto own = ::Api::SendAction(bot->owner().history(bot));
	if (info && !info->hasMainApp && !info->botMenuButtonUrl.isEmpty()) {
		// The server refuses a menu button URL in any other chat.
		bot->session().attachWebView().open({
			.bot = bot,
			.parentShow = std::move(show),
			.context = {
				.controller = controller,
				.action = own,
				.maySkipConfirmation = true,
				.svipeAttach = chat,
				.svipeSideMenu = !chat,
			},
			.button = {
				.text = info->botMenuButtonText,
				.url = info->botMenuButtonUrl.toUtf8(),
			},
			.source = InlineBots::WebViewSourceBotMenu(),
		});
	} else {
		bot->session().attachWebView().open({
			.bot = bot,
			.parentShow = std::move(show),
			.context = {
				.controller = controller,
				.action = chat.value_or(own),
				.maySkipConfirmation = true,
				.svipeAttach = chat,
				.svipeSideMenu = !chat,
			},
			.button = { .startCommand = startCommand },
			.source = InlineBots::WebViewSourceBotProfile(),
		});
	}
}

void SignContext(
		not_null<UserData*> bot,
		not_null<PeerData*> peer,
		const QString &url,
		Fn<void(QString url)> done) {
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=](QString result) {
		if (!std::exchange(*finished, true)) {
			done(result);
		}
	};
	if (!url.contains('#')) {
		finish(url);
		return;
	}
	const auto session = &bot->session();
	const auto body = ContextBody(bot, peer, url);
	base::call_delayed(kSignTimeout, session, [=] {
		finish(url);
	});
	Auth::EnsureToken(session, crl::guard(session, [=](QString token) {
		if (token.isEmpty()) {
			finish(url);
			return;
		}
		Api::Post(
			u"/v1/attach/context"_q,
			body,
			token,
			crl::guard(session, [=](QJsonObject result, int code) {
				const auto signed_ = (code == 200)
					? result.value(u"svipe_init_data"_q).toString()
					: QString();
				finish(signed_.isEmpty()
					? url
					: (url
						+ u"&tgWebAppSvipeData="_q
						+ QString::fromLatin1(
							QUrl::toPercentEncoding(signed_))));
			}));
	}));
}

void SendPrepared(
		not_null<UserData*> bot,
		const QString &id,
		::Api::SendAction action,
		Fn<void(QString error)> done) {
	const auto session = &bot->session();
	session->api().request(MTPmessages_GetPreparedInlineMessage(
		bot->inputUser(),
		MTP_string(id)
	)).done(crl::guard(session, [=](
			const MTPmessages_PreparedInlineMessage &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		const auto parsed = InlineBots::Result::Create(
			session,
			data.vquery_id().v,
			data.vresult());
		if (!parsed) {
			done(u"MESSAGE_EXPIRED"_q);
			return;
		} else if (!Allows(data.vpeer_types(), bot, action.history->peer)) {
			done(u"MESSAGE_SEND_FAILED"_q);
			return;
		}
		// As the share box sends a prepared message (botSendPreparedMessage).
		session->api().sendInlineResult(
			bot,
			parsed.get(),
			action,
			std::nullopt,
			[=](bool success) {
				[[maybe_unused]] const auto keep = parsed;
				done(success ? QString() : u"MESSAGE_SEND_FAILED"_q);
			});
	})).fail(crl::guard(session, [=] {
		done(u"MESSAGE_EXPIRED"_q);
	})).send();
}

} // namespace Svipe::AttachMenu
