/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_direct.h"

#include "api/api_text_entities.h"
#include "base/random.h"
#include "base/timer_rpl.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "styles/style_chat.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtSvg/QSvgRenderer>

namespace Svipe::Direct {
namespace {

const auto kOutboxKey = u"svipe_direct_outbox"_q;
constexpr auto kPollInterval = 10 * crl::time(1000);
constexpr auto kPageLimit = 200;

// The Svipe logo, cropped to the mark (svipe_direct_mark.xml on Android).
const auto kMarkSvg = QByteArray(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 66 66"><g transform="translate(-19.558 -19.599) scale(0.0023659)" fill="#fff" fill-rule="evenodd"><path d="M25498.76 29195.39c-155.24,51.32 -352.81,210.12 -500.07,305.27 -161.01,104.04 -330.92,210.65 -493.19,316.29 -323.82,210.79 -642.7,405.42 -960.32,618.99 -30.72,45.53 -8.19,34.27 -76.6,77.91 -91.28,58.23 124.3,-25.18 -90.23,31.6 -315.66,219.5 -2495.26,1635.6 -2703.64,1715.73 -80.8,140.01 -550.32,314.14 -775.15,387.98 -1700.73,558.53 -3198.1,-1044.91 -2784.81,-2856.86 95.9,-420.48 521.37,-1432.86 776.94,-1563.35 138.63,-412.96 1251.1,-1178.56 1795.36,-1432.27 304.23,-141.83 607.41,-350.91 662.4,-375.1 110.65,-152.41 326.14,-241.3 567.41,-739.01 182.41,-615.23 210.12,-691.5 34.15,-1322.6l-73.2 -100.18c-4.18,-7.88 -7.51,-16.91 -11.17,-24.86 -38.25,-83.18 -12.64,-87.16 -85.44,-186.66 -184.31,-251.93 -138.86,-96.96 -247.44,-414.87 -263.68,-251.93 -1502.65,-543.87 -2399.1,-1519.51 -251.73,-273.96 -580.91,-753.54 -768.11,-1202.96 -285.93,-686.46 -249.76,-905.71 -173.45,-1654.82 219.88,-431.56 260.79,-713.69 764.06,-1095.61 349.39,-265.15 865.2,-452.56 1414.83,-425.48 872.34,42.98 1721.93,683.06 2545.12,1161.05 914.5,531 6028.31,3655.28 6821.84,4277.45 614.9,482.1 1167.07,1535.95 795.43,2652.32 -329.28,989.13 -1058.29,1357.36 -1813.89,1885.56l-2221.73 1483.99zm-8308.5 -14239.2c-2071.96,7.53 -3978.48,-237.5 -5628.22,754.79 -620.68,373.33 -1275.94,898.62 -1693.34,1478.35 -1307.26,1815.61 -1230.68,2918.34 -1235.85,5097.82l-1.61 6342.34c-0.3,1068.42 -36.49,1890.44 266.35,2899 351.78,1171.56 1013.35,2182.91 1871.4,2919.76 1996.22,1714.24 3926.85,1406.39 6522.96,1404.64l9513.52 -1.29c643.54,0.98 2653.06,30.61 3142.13,-5.99 1472.8,-61.31 2961.53,-759.21 3889.63,-1769.75 827.44,-900.92 1558.8,-2166.59 1708.81,-3886.15 74.67,-856.02 17.93,-1914.91 19.87,-2793.47l0.9 -5602.39c0.22,-907.96 22.21,-1709.75 -212.99,-2558.24 -418.36,-1509.26 -1362.84,-2640.14 -2342.5,-3295.53 -1539.95,-1030.22 -2542.98,-978.88 -4620.26,-985.31 -3730.51,-11.55 -7470.46,-14.74 -11200.79,1.41z"/><path d="M8376.65 16888.12c99.45,-94.08 257.12,-431.91 364.47,-580.44 440.97,-610.09 766.96,-1011.05 1406.27,-1482.11 2052.58,-1512.42 3959.37,-1211.23 6475.09,-1203.69 877.87,2.63 3144.8,39.05 3869.27,-6.93l-3058.86 -5275.47c-909.55,-55.83 -3413.16,-48.64 -4247.06,152.74 -2135.4,515.67 -3822.25,2187.69 -4503.78,4304.37 -347.28,1078.56 -415.55,2994.25 -305.39,4091.53z"/><path d="M29056.31 13621.31c2316.89,39.76 3219.83,-64.34 5048.88,1225.17 1132.51,798.45 1619.77,1927.43 1751.49,2056.65 -33.89,-2455.16 197.45,-3997.47 -1339.86,-6097.73 -916.4,-1251.97 -2385.13,-2158.87 -3938.3,-2399.48 -698.48,-108.2 -1492.05,-75.87 -2222.71,-73.73 -773.44,2.26 -1575.77,19.66 -2345.91,0.1 56.85,189.17 1313.26,2292.78 1523.81,2654.68 379.52,652.32 1229.82,2058.04 1522.6,2634.35z"/><path d="M21974.63 13620.31l5606.57 1.45 -3041.43 -5276.25c-1076.67,-56 -3256.41,-15.06 -4429.01,-12.17l-1191.74 1.93c46.43,176.25 2007.28,3455.11 2280.03,3947.53 251,453.16 549.36,889.17 775.58,1337.5z"/></g></svg>)SVG");

struct Outgoing {
	PeerId peer = 0;
	QString clientId;
	QByteArray body;
};

struct State {
	qint64 cursor = 0;
	bool syncing = false;
	bool again = false;
	std::vector<Outgoing> outbox;
	base::flat_map<QString, FullMsgId> byClientId;
	base::flat_map<FullMsgId, QString> clientIdOf;
	base::flat_set<QString> gone; // withdrawn, or deleted here
	rpl::lifetime lifetime;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<State>> States;

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	const auto i = States.find(session);
	if (i != end(States)) {
		return *i->second;
	}
	session->lifetime().add([=] {
		States.remove(session);
	});
	return *States.emplace(
		session,
		std::make_unique<State>()).first->second;
}

[[nodiscard]] QString NewClientId() {
	auto bytes = QByteArray(16, Qt::Uninitialized);
	base::RandomFill(bytes.data(), bytes.size());
	return QString::fromLatin1(bytes.toHex());
}

[[nodiscard]] bool IsClientId(const QString &value) {
	static const auto re = QRegularExpression(u"^[0-9a-f]{32}$"_q);
	return re.match(value).hasMatch();
}

// ---- wire format (shared with Android SvipeDirect.encodeBody / decodeBody) ----

[[nodiscard]] QJsonArray EntitiesToJson(const EntitiesInText &entities) {
	auto result = QJsonArray();
	for (const auto &e : entities) {
		auto j = QJsonObject();
		auto t = QString();
		switch (e.type()) {
		case EntityType::Bold: t = u"bold"_q; break;
		case EntityType::Italic: t = u"italic"_q; break;
		case EntityType::Underline: t = u"underline"_q; break;
		case EntityType::StrikeOut: t = u"strike"_q; break;
		case EntityType::Spoiler: t = u"spoiler"_q; break;
		case EntityType::Code: t = u"code"_q; break;
		case EntityType::Pre:
			t = u"pre"_q;
			if (!e.data().isEmpty()) {
				j.insert(u"lang"_q, e.data());
			}
			break;
		case EntityType::CustomUrl:
			t = u"text_url"_q;
			j.insert(u"url"_q, e.data());
			break;
		case EntityType::Blockquote:
			t = u"blockquote"_q;
			if (!e.data().isEmpty()) {
				j.insert(u"collapsed"_q, true);
			}
			break;
		case EntityType::CustomEmoji: {
			const auto id = Data::ParseCustomEmojiData(e.data());
			if (!id) {
				continue;
			}
			t = u"custom_emoji"_q;
			j.insert(u"doc"_q, QString::number(id)); // string: a double loses 64-bit ids
		} break;
		case EntityType::MentionName: {
			const auto fields = TextUtilities::MentionNameDataToFields(
				e.data());
			if (!fields.userId) {
				continue;
			}
			t = u"mention_name"_q;
			j.insert(u"user"_q, double(fields.userId));
		} break;
		case EntityType::Url: t = u"url"_q; break;
		case EntityType::Email: t = u"email"_q; break;
		case EntityType::Mention: t = u"mention"_q; break;
		case EntityType::Hashtag: t = u"hashtag"_q; break;
		case EntityType::Cashtag: t = u"cashtag"_q; break;
		case EntityType::BotCommand: t = u"bot_command"_q; break;
		case EntityType::Phone: t = u"phone"_q; break;
		default: continue;
		}
		j.insert(u"t"_q, t);
		j.insert(u"o"_q, e.offset());
		j.insert(u"l"_q, e.length());
		result.push_back(j);
	}
	return result;
}

[[nodiscard]] EntitiesInText EntitiesFromJson(
		not_null<Main::Session*> session,
		const QJsonArray &list,
		int textLength) {
	auto result = EntitiesInText();
	for (const auto &value : list) {
		const auto j = value.toObject();
		const auto t = j.value(u"t"_q).toString();
		const auto o = j.value(u"o"_q).toInt(-1);
		const auto l = j.value(u"l"_q).toInt(0);
		if (o < 0 || l <= 0 || o + l > textLength) {
			continue;
		}
		auto type = EntityType::Invalid;
		auto data = QString();
		if (t == u"bold"_q) type = EntityType::Bold;
		else if (t == u"italic"_q) type = EntityType::Italic;
		else if (t == u"underline"_q) type = EntityType::Underline;
		else if (t == u"strike"_q) type = EntityType::StrikeOut;
		else if (t == u"spoiler"_q) type = EntityType::Spoiler;
		else if (t == u"code"_q) type = EntityType::Code;
		else if (t == u"pre"_q) {
			type = EntityType::Pre;
			data = j.value(u"lang"_q).toString();
		} else if (t == u"text_url"_q) {
			type = EntityType::CustomUrl;
			data = j.value(u"url"_q).toString();
		} else if (t == u"blockquote"_q) {
			type = EntityType::Blockquote;
			data = j.value(u"collapsed"_q).toBool() ? u"1"_q : QString();
		} else if (t == u"custom_emoji"_q) {
			const auto id = j.value(u"doc"_q).toString().toULongLong();
			if (!id) {
				continue;
			}
			type = EntityType::CustomEmoji;
			data = Data::SerializeCustomEmojiId(id);
		} else if (t == u"mention_name"_q) {
			const auto user = uint64(j.value(u"user"_q).toDouble());
			if (!user) {
				continue;
			}
			type = EntityType::MentionName;
			data = TextUtilities::MentionNameDataFromFields({
				.selfId = session->userId().bare,
				.userId = user,
			});
		} else if (t == u"url"_q) type = EntityType::Url;
		else if (t == u"email"_q) type = EntityType::Email;
		else if (t == u"mention"_q) type = EntityType::Mention;
		else if (t == u"hashtag"_q) type = EntityType::Hashtag;
		else if (t == u"cashtag"_q) type = EntityType::Cashtag;
		else if (t == u"bot_command"_q) type = EntityType::BotCommand;
		else if (t == u"phone"_q) type = EntityType::Phone;
		else continue;
		result.push_back(EntityInText(type, o, l, data));
	}
	return result;
}

struct Fwd {
	PeerId from = 0;
	QString name;
	TimeId date = 0;
};

struct Body {
	TextWithEntities text;
	QString replyClientId;
	std::optional<Fwd> fwd;
};

[[nodiscard]] QByteArray EncodeBody(
		const TextWithEntities &text,
		const QString &replyClientId,
		const std::optional<Fwd> &fwd) {
	auto o = QJsonObject();
	o.insert(u"v"_q, 1);
	o.insert(u"text"_q, text.text);
	const auto entities = EntitiesToJson(text.entities);
	if (!entities.isEmpty()) {
		o.insert(u"entities"_q, entities);
	}
	if (!replyClientId.isEmpty()) {
		o.insert(u"reply_to"_q, replyClientId);
	}
	if (fwd) {
		auto f = QJsonObject();
		if (peerIsUser(fwd->from)) {
			f.insert(u"user"_q, double(peerToUser(fwd->from).bare));
		} else if (peerIsChannel(fwd->from)) {
			f.insert(u"channel"_q, double(peerToChannel(fwd->from).bare));
		}
		if (!fwd->name.isEmpty()) {
			f.insert(u"name"_q, fwd->name);
		}
		f.insert(u"date"_q, fwd->date);
		o.insert(u"fwd"_q, f);
	}
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::optional<Body> DecodeBody(
		not_null<Main::Session*> session,
		const QByteArray &bytes) {
	const auto document = QJsonDocument::fromJson(bytes);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto o = document.object();
	auto result = Body();
	result.text.text = o.value(u"text"_q).toString();
	if (result.text.text.isEmpty()) {
		return std::nullopt;
	}
	result.text.entities = EntitiesFromJson(
		session,
		o.value(u"entities"_q).toArray(),
		result.text.text.size());
	const auto reply = o.value(u"reply_to"_q).toString();
	if (IsClientId(reply)) {
		result.replyClientId = reply;
	}
	if (o.contains(u"fwd"_q)) {
		const auto f = o.value(u"fwd"_q).toObject();
		auto fwd = Fwd();
		if (f.contains(u"user"_q)) {
			fwd.from = peerFromUser(UserId(uint64(f.value(u"user"_q).toDouble())));
		} else if (f.contains(u"channel"_q)) {
			fwd.from = peerFromChannel(
				ChannelId(uint64(f.value(u"channel"_q).toDouble())));
		}
		fwd.name = f.value(u"name"_q).toString();
		fwd.date = f.value(u"date"_q).toInt();
		if (fwd.from || !fwd.name.isEmpty()) {
			result.fwd = fwd;
		}
	}
	return result;
}

// ---- items ----

[[nodiscard]] MTPMessage BuildMessage(
		not_null<History*> history,
		MsgId id,
		PeerId from,
		bool out,
		TimeId date,
		const TextWithEntities &text,
		MsgId replyTo,
		const std::optional<Fwd> &fwd) {
	using Flag = MTPDmessage::Flag;
	const auto session = &history->session();
	const auto entities = ::Api::EntitiesToMTP(
		session,
		text.entities,
		::Api::ConvertOption::SkipLocal);
	const auto flags = Flag::f_from_id
		| (out ? Flag::f_out : Flag())
		| (entities.v.isEmpty() ? Flag() : Flag::f_entities)
		| (fwd ? Flag::f_fwd_from : Flag())
		| (replyTo ? Flag::f_reply_to : Flag());
	const auto fwdHeader = [&]() -> MTPMessageFwdHeader {
		if (!fwd) {
			return MTPMessageFwdHeader();
		}
		using FwdFlag = MTPDmessageFwdHeader::Flag;
		return MTP_messageFwdHeader(
			MTP_flags((fwd->from ? FwdFlag::f_from_id : FwdFlag())
				| (fwd->name.isEmpty() ? FwdFlag() : FwdFlag::f_from_name)),
			fwd->from ? peerToMTP(fwd->from) : MTPPeer(),
			MTP_string(fwd->name),
			MTP_int(fwd->date),
			MTPint(), // channel_post
			MTPstring(), // post_author
			MTPPeer(), // saved_from_peer
			MTPint(), // saved_from_msg_id
			MTPPeer(), // saved_from_id
			MTPstring(), // saved_from_name
			MTPint(), // saved_date
			MTPstring()); // psa_type
	}();
	const auto replyHeader = replyTo
		? MTP_messageReplyHeader(
			MTP_flags(MTPDmessageReplyHeader::Flag::f_reply_to_msg_id),
			MTP_int(replyTo.bare),
			MTPPeer(),
			MTPMessageFwdHeader(),
			MTPMessageMedia(),
			MTPint(), // reply_to_top_id
			MTPstring(), // quote_text
			MTPVector<MTPMessageEntity>(),
			MTPint(), // quote_offset
			MTPint(), // todo_item_id
			MTPbytes(), // poll_option
			MTPVector<MTPRichText>()) // quote_rich
		: MTPMessageReplyHeader();
	return MTP_message(
		MTP_flags(flags),
		MTP_int(id.bare),
		peerToMTP(from),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(history->peer->id),
		MTPPeer(), // saved_peer_id
		fwdHeader,
		MTPlong(), // via_bot_id
		MTPlong(), // via_business_bot_id
		MTPPeer(), // guestchat_via_from
		replyHeader,
		MTP_int(date),
		MTP_string(text.text),
		MTPMessageMedia(),
		MTPReplyMarkup(),
		entities,
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTPint(), // edit_date
		MTPstring(), // post_author
		MTPlong(), // grouped_id
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(), // ttl_period
		MTPint(), // quick_reply_shortcut_id
		MTPlong(), // effect
		MTPFactCheck(),
		MTPint(), // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(), // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
}

HistoryItem *Show(
		not_null<History*> history,
		const QString &clientId,
		PeerId from,
		bool out,
		TimeId date,
		const TextWithEntities &text,
		const QString &replyClientId,
		const std::optional<Fwd> &fwd) {
	const auto session = &history->session();
	auto &state = StateFor(session);
	if (state.byClientId.contains(clientId) || state.gone.contains(clientId)) {
		return nullptr;
	}
	auto replyTo = MsgId();
	if (!replyClientId.isEmpty()) {
		const auto i = state.byClientId.find(replyClientId);
		if (i != end(state.byClientId)) {
			replyTo = i->second.msg;
		}
	}
	const auto id = session->data().nextLocalMessageId();
	const auto item = history->createItem(
		id,
		BuildMessage(history, id, from, out, date, text, replyTo, fwd),
		// Never on Telegram's servers: keeps it out of read requests and every other server call.
		MessageFlag::Local);
	state.byClientId.emplace(clientId, item->fullId());
	state.clientIdOf.emplace(item->fullId(), clientId);
	history->checkLocalMessages();
	return item;
}

// ---- outbox ----

void SaveOutbox(not_null<Main::Session*> session) {
	auto list = QJsonArray();
	for (const auto &entry : StateFor(session).outbox) {
		list.push_back(QJsonObject{
			{ u"peer"_q, double(peerToUser(entry.peer).bare) },
			{ u"client_id"_q, entry.clientId },
			{ u"body"_q, QString::fromLatin1(entry.body.toBase64()) },
		});
	}
	if (list.isEmpty()) {
		Storage::Remove(session, kOutboxKey);
	} else {
		Storage::Set(session, kOutboxKey, list);
	}
}

void LoadOutbox(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	for (const auto &value : Storage::Get(session, kOutboxKey).toArray()) {
		const auto o = value.toObject();
		auto entry = Outgoing{
			.peer = peerFromUser(UserId(uint64(o.value(u"peer"_q).toDouble()))),
			.clientId = o.value(u"client_id"_q).toString(),
			.body = QByteArray::fromBase64(o.value(u"body"_q).toString().toLatin1()),
		};
		if (entry.peer && IsClientId(entry.clientId) && !entry.body.isEmpty()) {
			state.outbox.push_back(std::move(entry));
		}
	}
}

void Sync(not_null<Main::Session*> session);

void FinishSync(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	state.syncing = false;
	if (std::exchange(state.again, false)) {
		Sync(session);
	}
}

void ApplyUpdates(not_null<Main::Session*> session, const QJsonArray &items) {
	auto &state = StateFor(session);
	const auto self = session->userId().bare;
	for (const auto &value : items) {
		const auto it = value.toObject();
		const auto clientId = it.value(u"client_id"_q).toString();
		const auto from = uint64(it.value(u"from"_q).toDouble());
		const auto to = uint64(it.value(u"to"_q).toDouble());
		if (!IsClientId(clientId) || (from != self && to != self)) {
			continue;
		}
		const auto out = (from == self);
		const auto peer = peerFromUser(UserId(out ? to : from));
		if (it.value(u"deleted"_q).toBool()) {
			state.gone.emplace(clientId);
			const auto i = state.byClientId.find(clientId);
			if (i != end(state.byClientId)) {
				const auto id = i->second;
				state.byClientId.erase(i);
				state.clientIdOf.remove(id);
				if (const auto item = session->data().message(id)) {
					item->destroy();
				}
			}
			continue;
		}
		if (state.byClientId.contains(clientId)
			|| state.gone.contains(clientId)) {
			continue; // my own, already on screen
		}
		const auto body = DecodeBody(
			session,
			QByteArray::fromBase64(
				it.value(u"body_b64"_q).toString().toLatin1()));
		if (!body) {
			continue;
		}
		Show(
			session->data().history(peer),
			clientId,
			peerFromUser(UserId(from)),
			out,
			TimeId(it.value(u"date"_q).toInt()),
			body->text,
			body->replyClientId,
			body->fwd);
	}
}

void FetchUpdates(not_null<Main::Session*> session, const QString &token) {
	const auto after = StateFor(session).cursor;
	Api::Get(
		u"/v1/direct/updates?after=%1&limit=%2"_q.arg(after).arg(kPageLimit),
		token,
		crl::guard(session, [=](QJsonObject result, int code) {
			if (code != 200) {
				FinishSync(session);
				return;
			}
			ApplyUpdates(session, result.value(u"items"_q).toArray());
			auto &state = StateFor(session);
			const auto next = qint64(result.value(u"next"_q).toDouble());
			if (next > state.cursor) {
				state.cursor = next;
			}
			if (result.value(u"more"_q).toBool()) {
				FetchUpdates(session, token);
			} else {
				FinishSync(session);
			}
		}));
}

void FlushOutbox(
		not_null<Main::Session*> session,
		const QString &token,
		Fn<void()> next) {
	auto &state = StateFor(session);
	if (state.outbox.empty()) {
		next();
		return;
	}
	const auto entry = state.outbox.front();
	Api::Post(
		u"/v1/direct/send"_q,
		QJsonObject{
			{ u"peer_tg_id"_q, double(peerToUser(entry.peer).bare) },
			{ u"client_id"_q, entry.clientId },
			{ u"body_b64"_q, QString::fromLatin1(entry.body.toBase64()) },
		},
		token,
		crl::guard(session, [=](QJsonObject, int code) {
			auto &state = StateFor(session);
			if (code == 200 || code == 400 || code == 413) {
				// Delivered, or the server will never take it: stop retrying either way.
				const auto i = ranges::find(
					state.outbox,
					entry.clientId,
					&Outgoing::clientId);
				if (i != end(state.outbox)) {
					state.outbox.erase(i);
				}
				SaveOutbox(session);
				FlushOutbox(session, token, next);
			} else {
				next(); // offline / 429 / 5xx: keep the order, retry next tick
			}
		}));
}

void Sync(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	if (state.syncing) {
		state.again = true;
		return;
	}
	state.syncing = true;
	Auth::EnsureToken(session, crl::guard(session, [=](QString token) {
		if (token.isEmpty()) {
			FinishSync(session);
			return;
		}
		FlushOutbox(session, token, crl::guard(session, [=] {
			FetchUpdates(session, token);
		}));
	}));
}

void Enqueue(
		not_null<History*> history,
		const TextWithEntities &text,
		const QString &replyClientId,
		const std::optional<Fwd> &fwd) {
	const auto session = &history->session();
	const auto clientId = NewClientId();
	Show(
		history,
		clientId,
		session->userPeerId(),
		true,
		base::unixtime::now(),
		text,
		replyClientId,
		fwd);
	StateFor(session).outbox.push_back({
		.peer = history->peer->id,
		.clientId = clientId,
		.body = EncodeBody(text, replyClientId, fwd),
	});
	SaveOutbox(session);
	Sync(session);
}

[[nodiscard]] QImage MarkImage(int size, const QColor &color) {
	static auto cache = base::flat_map<std::pair<int, QRgb>, QImage>();
	const auto ratio = style::DevicePixelRatio();
	const auto key = std::make_pair(size * ratio, color.rgba());
	if (const auto i = cache.find(key); i != end(cache)) {
		return i->second;
	}
	auto image = QImage(
		QSize(size, size) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(ratio);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::Antialiasing);
		auto renderer = QSvgRenderer(kMarkSvg);
		renderer.render(&p, QRectF(0, 0, size, size));
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(QRect(0, 0, size, size), color);
	}
	cache.emplace(key, image);
	return image;
}

[[nodiscard]] int MarkSize() {
	return st::msgDateFont->ascent - st::lineWidth;
}

} // namespace

void Start(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	LoadOutbox(session);
	base::timer_each(
		kPollInterval
	) | rpl::on_next([=] {
		Sync(session);
	}, state.lifetime);
	Sync(session);
}

void OnChatOpened(not_null<History*> history) {
	if (CanSendTo(history->peer)) {
		Sync(&history->session());
	}
}

bool CanSendTo(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user
		&& !user->isBot()
		&& !user->isSelf()
		&& !user->isServiceUser()
		&& !user->isInaccessible();
}

bool IsDirect(not_null<const HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto i = States.find(session);
	return (i != end(States))
		&& i->second->clientIdOf.contains(item->fullId());
}

void Send(
		not_null<History*> history,
		TextWithEntities text,
		HistoryItem *replyTo) {
	if (!CanSendTo(history->peer) || text.text.isEmpty()) {
		return;
	}
	auto replyClientId = QString();
	if (replyTo) {
		const auto &state = StateFor(&history->session());
		const auto i = state.clientIdOf.find(replyTo->fullId());
		if (i != end(state.clientIdOf)) {
			replyClientId = i->second;
		}
	}
	Enqueue(history, text, replyClientId, std::nullopt);
}

void Forward(
		not_null<History*> to,
		not_null<HistoryItem*> item,
		bool dropAuthor) {
	if (!CanSendTo(to->peer)) {
		Ui::Toast::Show(Tr(Str::DirectForwardOnlyPrivate));
		return;
	}
	const auto text = item->originalText();
	if (text.text.isEmpty()) {
		return;
	}
	auto fwd = std::optional<Fwd>();
	if (!dropAuthor) {
		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			fwd = Fwd{
				.from = forwarded->originalSender
					? forwarded->originalSender->id
					: PeerId(),
				.name = forwarded->originalHiddenSenderInfo
					? forwarded->originalHiddenSenderInfo->name
					: QString(),
				.date = forwarded->originalDate,
			};
		} else {
			fwd = Fwd{ .from = item->from()->id, .date = item->date() };
		}
	}
	Enqueue(to, text, QString(), fwd);
}

void OnDelete(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	auto &state = StateFor(session);
	const auto i = state.clientIdOf.find(item->fullId());
	if (i == end(state.clientIdOf)) {
		return;
	}
	const auto clientId = i->second;
	state.clientIdOf.erase(i);
	state.byClientId.remove(clientId);
	state.gone.emplace(clientId);
	const auto pending = ranges::find(
		state.outbox,
		clientId,
		&Outgoing::clientId);
	if (pending != end(state.outbox)) {
		state.outbox.erase(pending); // never left this computer
		SaveOutbox(session);
		return;
	}
	if (item->out()) {
		Auth::EnsureToken(session, [=](QString token) {
			if (!token.isEmpty()) {
				Api::Delete(
					u"/v1/direct/messages/"_q + clientId,
					token,
					[](QJsonObject, int) {});
			}
		});
	}
}

int MarkWidth() {
	return MarkSize() + st::msgDateFont->spacew;
}

void PaintMark(QPainter &p, int left, int top, const QColor &color) {
	const auto size = MarkSize();
	p.drawImage(
		QPoint(left, top + (st::msgDateFont->height - size) / 2),
		MarkImage(size, color));
}

QString SendLabel() {
	return Tr(Str::DirectSend);
}

QString MenuHint() {
	return Tr(Str::DirectMenuHint);
}

} // namespace Svipe::Direct
