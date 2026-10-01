/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_message_archive.h"

#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"
#include "settings.h"

#include <QtCore/QDir>

namespace Svipe::MessageArchive {
namespace {

constexpr auto kFormat = quint32(1);

// Android evicts at ~2000 entries per chat; the same cap here keeps a chat file around a megabyte.
constexpr auto kMaxEntriesPerChat = 2000;

// Remembered MTP copies, oldest dropped first. A message is ~0.3-1 KB of TL, so this bounds the
// memory at a few tens of megabytes even for an account that scrolls through a lot.
constexpr auto kMaxRemembered = 60000;

struct Remembered {
	QByteArray bytes;
	TimeId editDate = 0;
};

struct State {
	base::flat_map<FullMsgId, Remembered> remembered;
	std::deque<FullMsgId> order;
	base::flat_map<PeerId, std::vector<Entry>> cache;
	rpl::event_stream<PeerId> updates;
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

[[nodiscard]] QString BasePath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/svipe/archive_%1/"_q.arg(session->userId().bare);
}

[[nodiscard]] QString FileName(PeerId peer) {
	return u"c%1"_q.arg(peer.value, 0, 16);
}

[[nodiscard]] QByteArray Serialize(const MTPMessage &message) {
	auto buffer = mtpBuffer();
	message.write(buffer);
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * sizeof(mtpPrime));
}

[[nodiscard]] TimeId EditDateOf(const MTPMessage &message) {
	return message.match([](const MTPDmessage &data) {
		return data.vedit_date().value_or_empty();
	}, [](const auto &) {
		return TimeId();
	});
}

[[nodiscard]] TimeId DateOf(const MTPMessage &message) {
	return message.match([](const MTPDmessage &data) {
		return data.vdate().v;
	}, [](const auto &) {
		return TimeId();
	});
}

[[nodiscard]] std::vector<Entry> ReadFile(
		not_null<Main::Session*> session,
		PeerId peer) {
	using namespace Storage::details;
	const auto key = session->local().peekLegacyLocalKey();
	if (!key) {
		return {};
	}
	auto file = FileReadDescriptor();
	if (!ReadEncryptedFile(file, FileName(peer), BasePath(session), key)) {
		return {};
	}
	auto format = quint32();
	auto count = quint32();
	file.stream >> format >> count;
	if (file.stream.status() != QDataStream::Ok || format != kFormat) {
		return {};
	}
	auto result = std::vector<Entry>();
	result.reserve(std::min(count, quint32(kMaxEntriesPerChat)));
	for (auto i = quint32(); i != count; ++i) {
		auto kind = qint32();
		auto date = qint32();
		auto editDate = qint32();
		auto archivedAt = qint32();
		auto id = qint64();
		auto mergeKey = QString();
		auto message = QByteArray();
		file.stream
			>> kind
			>> date
			>> editDate
			>> archivedAt
			>> id
			>> mergeKey
			>> message;
		if (file.stream.status() != QDataStream::Ok) {
			break;
		}
		result.push_back({
			.kind = Kind(kind),
			.date = date,
			.editDate = editDate,
			.archivedAt = archivedAt,
			.id = MsgId(id),
			.mergeKey = mergeKey,
			.message = message,
		});
	}
	return result;
}

void WriteFile(
		not_null<Main::Session*> session,
		PeerId peer,
		const std::vector<Entry> &entries) {
	using namespace Storage::details;
	const auto key = session->local().peekLegacyLocalKey();
	if (!key) {
		return;
	}
	const auto base = BasePath(session);
	QDir().mkpath(base);
	auto size = 2 * sizeof(quint32);
	for (const auto &entry : entries) {
		size += 4 * sizeof(qint32) + sizeof(qint64)
			+ sizeof(quint32) + entry.mergeKey.size() * sizeof(ushort)
			+ sizeof(quint32) + entry.message.size();
	}
	auto data = EncryptedDescriptor(size);
	data.stream << kFormat << quint32(entries.size());
	for (const auto &entry : entries) {
		data.stream
			<< qint32(entry.kind)
			<< qint32(entry.date)
			<< qint32(entry.editDate)
			<< qint32(entry.archivedAt)
			<< qint64(entry.id.bare)
			<< entry.mergeKey
			<< entry.message;
	}
	auto file = FileWriteDescriptor(FileName(peer), base);
	file.writeEncrypted(data, key);
}

[[nodiscard]] std::vector<Entry> &Cached(
		not_null<Main::Session*> session,
		PeerId peer) {
	auto &state = StateFor(session);
	const auto i = state.cache.find(peer);
	if (i != end(state.cache)) {
		return i->second;
	}
	return state.cache.emplace(peer, ReadFile(session, peer)).first->second;
}

void Append(
		not_null<Main::Session*> session,
		PeerId peer,
		Entry entry) {
	auto &entries = Cached(session, peer);
	entries.push_back(std::move(entry));
	if (entries.size() > kMaxEntriesPerChat) {
		entries.erase(
			begin(entries),
			begin(entries) + (entries.size() - kMaxEntriesPerChat));
	}
	WriteFile(session, peer, entries);
	StateFor(session).updates.fire_copy(peer);
}

[[nodiscard]] Remembered *FindRemembered(not_null<HistoryItem*> item) {
	auto &state = StateFor(&item->history()->session());
	const auto i = state.remembered.find(item->fullId());
	return (i != end(state.remembered)) ? &i->second : nullptr;
}

} // namespace

void Remember(not_null<History*> history, MsgId id, const MTPMessage &message) {
	if (message.type() != mtpc_message || !IsServerMsgId(id)) {
		return;
	}
	auto &state = StateFor(&history->session());
	const auto key = FullMsgId(history->peer->id, id);
	auto &slot = state.remembered[key];
	if (slot.bytes.isEmpty()) {
		state.order.push_back(key);
		while (state.order.size() > kMaxRemembered) {
			state.remembered.remove(state.order.front());
			state.order.pop_front();
		}
	}
	slot.bytes = Serialize(message);
	slot.editDate = EditDateOf(message);
}

void RememberExisting(
		not_null<Main::Session*> session,
		const MTPMessage &message) {
	if (message.type() != mtpc_message) {
		return;
	}
	const auto &data = message.c_message();
	const auto peer = peerFromMTP(data.vpeer_id());
	if (const auto item = session->data().message(peer, data.vid().v)) {
		Remember(item->history(), item->id, message);
	}
}

void RememberSent(not_null<HistoryItem*> item) {
	if (!item->isRegular() || FindRemembered(item)) {
		return;
	}
	const auto session = &item->history()->session();
	const auto text = item->originalText();
	using Flag = MTPDmessage::Flag;
	const auto flags = Flag::f_from_id
		| (item->out() ? Flag::f_out : Flag())
		| (text.entities.isEmpty() ? Flag() : Flag::f_entities);
	Remember(item->history(), item->id, MTP_message(
		MTP_flags(flags),
		MTP_int(item->id.bare),
		peerToMTP(item->from()->id),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(item->history()->peer->id),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(),
		MTPlong(), // via_bot_id
		MTPlong(), // via_business_bot_id
		MTPPeer(), // guestchat_via_from
		MTPMessageReplyHeader(),
		MTP_int(item->date()),
		MTP_string(text.text),
		MTP_messageMediaEmpty(),
		MTPReplyMarkup(),
		Api::EntitiesToMTP(session, text.entities, Api::ConvertOption::SkipLocal),
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTPint(), // edit_date
		MTPstring(),
		MTPlong(),
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
		MTPRichMessage()));
}

void CaptureDeleted(not_null<HistoryItem*> item) {
	const auto remembered = FindRemembered(item);
	if (!remembered) {
		return;
	}
	const auto session = &item->history()->session();
	Append(session, item->history()->peer->id, {
		.kind = Kind::Deleted,
		.date = item->date(),
		.editDate = remembered->editDate,
		.archivedAt = base::unixtime::now(),
		.id = item->id,
		.message = remembered->bytes,
	});
	StateFor(session).remembered.remove(item->fullId());
}

void CaptureDeleted(const std::vector<not_null<HistoryItem*>> &items) {
	for (const auto &item : items) {
		CaptureDeleted(item);
	}
}

void CaptureEdited(not_null<HistoryItem*> item, const MTPMessage &edited) {
	if (edited.type() != mtpc_message) {
		return;
	}
	const auto remembered = FindRemembered(item);
	const auto newEditDate = EditDateOf(edited);
	// Webpage previews and reactions arrive through the same update with an unchanged edit date;
	// only a real edit moves it forward.
	if (remembered && newEditDate > remembered->editDate) {
		Append(&item->history()->session(), item->history()->peer->id, {
			.kind = Kind::EditedPrior,
			.date = DateOf(edited),
			.editDate = remembered->editDate,
			.archivedAt = base::unixtime::now(),
			.id = item->id,
			.message = remembered->bytes,
		});
	}
	Remember(item->history(), item->id, edited);
}

void AddSynced(
		not_null<Main::Session*> session,
		PeerId peer,
		std::vector<Entry> synced) {
	auto &entries = Cached(session, peer);
	auto known = base::flat_set<QString>();
	auto messages = base::flat_set<QByteArray>();
	for (const auto &entry : entries) {
		if (!entry.mergeKey.isEmpty()) {
			known.emplace(entry.mergeKey);
		}
		messages.emplace(entry.message);
	}
	auto added = false;
	for (auto &entry : synced) {
		if (known.contains(entry.mergeKey) || messages.contains(entry.message)) {
			continue;
		}
		known.emplace(entry.mergeKey);
		messages.emplace(entry.message);
		entries.push_back(std::move(entry));
		added = true;
	}
	if (!added) {
		return;
	}
	ranges::stable_sort(entries, ranges::less(), [](const Entry &entry) {
		return std::max(entry.editDate, entry.date);
	});
	if (entries.size() > kMaxEntriesPerChat) {
		entries.erase(
			begin(entries),
			begin(entries) + (entries.size() - kMaxEntriesPerChat));
	}
	WriteFile(session, peer, entries);
	StateFor(session).updates.fire_copy(peer);
}

void SetMergeKey(
		not_null<Main::Session*> session,
		PeerId peer,
		const QByteArray &message,
		const QString &mergeKey) {
	auto &entries = Cached(session, peer);
	auto changed = false;
	for (auto &entry : entries) {
		if (entry.message == message && entry.mergeKey != mergeKey) {
			entry.mergeKey = mergeKey;
			changed = true;
		}
	}
	if (changed) {
		WriteFile(session, peer, entries);
	}
}

std::vector<Entry> Load(not_null<Main::Session*> session, PeerId peer) {
	return Cached(session, peer);
}

bool HasEntries(not_null<Main::Session*> session, PeerId peer) {
	return !Cached(session, peer).empty();
}

rpl::producer<PeerId> Updates(not_null<Main::Session*> session) {
	return StateFor(session).updates.events();
}

QByteArray LiveBytes(not_null<HistoryItem*> item) {
	const auto remembered = FindRemembered(item);
	return remembered ? remembered->bytes : QByteArray();
}

std::optional<MTPMessage> Parse(const QByteArray &bytes) {
	if (bytes.isEmpty() || (bytes.size() % sizeof(mtpPrime))) {
		return std::nullopt;
	}
	auto from = reinterpret_cast<const mtpPrime*>(bytes.constData());
	const auto end = from + (bytes.size() / sizeof(mtpPrime));
	auto result = MTPMessage();
	if (!result.read(from, end) || result.type() != mtpc_message) {
		return std::nullopt;
	}
	return result;
}

} // namespace Svipe::MessageArchive
