/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_deleted_in_chat.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "svipe/svipe_message_archive.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"

namespace Svipe::DeletedInChat {
namespace {

const auto kEnabledPrefix = u"svipe_show_deleted_"_q;

struct State {
	base::flat_set<FullMsgId> marked;
	base::flat_map<PeerId, std::vector<FullMsgId>> restored;
	base::flat_map<PeerId, base::flat_set<QByteArray>> restoredBytes;
	rpl::event_stream<PeerId> enabledChanges;
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

[[nodiscard]] QString EnabledKey(PeerId peer) {
	return kEnabledPrefix + QString::number(peer.value);
}

// The archived copy with the bubble side set from who wrote it: a synced copy of my message was
// uploaded by the other side, where it was incoming. Views, replies and grouping are dropped — a
// restored message must not join a live album or count as a thread.
[[nodiscard]] MTPMessage ForChat(const MTPDmessage &data, bool out) {
	using Flag = MTPDmessage::Flag;
	const auto removeFlags = Flag::f_out
		| Flag::f_replies
		| Flag::f_grouped_id
		| Flag::f_views
		| Flag::f_forwards
		| Flag::f_reactions
		| Flag::f_ttl_period
		| Flag::f_report_delivery_until_date
		| Flag::f_suggested_post;
	const auto flags = (data.vflags().v & ~removeFlags)
		| (out ? Flag::f_out : Flag());
	return MTP_message(
		MTP_flags(flags),
		data.vid(),
		data.vfrom_id() ? *data.vfrom_id() : MTPPeer(),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		data.vpeer_id(),
		data.vsaved_peer_id() ? *data.vsaved_peer_id() : MTPPeer(),
		data.vfwd_from() ? *data.vfwd_from() : MTPMessageFwdHeader(),
		MTP_long(data.vvia_bot_id().value_or_empty()),
		MTP_long(data.vvia_business_bot_id().value_or_empty()),
		(data.vguestchat_via_from()
			? *data.vguestchat_via_from()
			: MTPPeer()),
		data.vreply_to() ? *data.vreply_to() : MTPMessageReplyHeader(),
		data.vdate(),
		data.vmessage(),
		data.vmedia() ? *data.vmedia() : MTPMessageMedia(),
		data.vreply_markup() ? *data.vreply_markup() : MTPReplyMarkup(),
		(data.ventities()
			? *data.ventities()
			: MTPVector<MTPMessageEntity>()),
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTP_int(data.vedit_date().value_or_empty()),
		data.vpost_author() ? *data.vpost_author() : MTPstring(),
		MTP_long(0), // grouped_id
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

void Unrestore(not_null<History*> history) {
	const auto session = &history->session();
	auto &state = StateFor(session);
	const auto peer = history->peer->id;
	auto ids = std::vector<FullMsgId>();
	if (const auto i = state.restored.find(peer); i != end(state.restored)) {
		ids = std::move(i->second);
		state.restored.erase(i);
	}
	state.restoredBytes.remove(peer);
	// Messages kept on deletion are gone on the server too, so they leave with the restored ones.
	for (const auto &id : state.marked) {
		if (id.peer == peer) {
			ids.push_back(id);
		}
	}
	for (const auto &id : ids) {
		state.marked.remove(id);
		if (const auto item = session->data().message(id)) {
			item->destroy();
		}
	}
}

} // namespace

void Start(not_null<Main::Session*> session) {
	MessageArchive::Updates(
		session
	) | rpl::on_next([=](PeerId peer) {
		if (const auto history = session->data().historyLoaded(peer)) {
			Restore(history);
		}
	}, session->lifetime());
}

bool Enabled(not_null<Main::Session*> session, PeerId peer) {
	return Storage::Get(session, EnabledKey(peer)).toBool();
}

rpl::producer<bool> EnabledValue(
		not_null<Main::Session*> session,
		PeerId peer) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		StateFor(session).enabledChanges.events(
		) | rpl::filter(rpl::mappers::_1 == peer) | rpl::to_empty
	) | rpl::map([=] {
		return Enabled(session, peer);
	});
}

void SetEnabled(not_null<History*> history, bool enabled) {
	const auto session = &history->session();
	const auto peer = history->peer->id;
	if (Enabled(session, peer) == enabled) {
		return;
	}
	if (enabled) {
		Storage::Set(session, EnabledKey(peer), true);
		Restore(history);
	} else {
		Storage::Remove(session, EnabledKey(peer));
		Unrestore(history);
	}
	StateFor(session).enabledChanges.fire_copy(peer);
}

std::vector<not_null<HistoryItem*>> KeepOrDestroy(
		std::vector<not_null<HistoryItem*>> items) {
	auto result = std::vector<not_null<HistoryItem*>>();
	result.reserve(items.size());
	for (const auto &item : items) {
		const auto history = item->history();
		const auto session = &history->session();
		if (item->isRegular() && Enabled(session, history->peer->id)) {
			StateFor(session).marked.emplace(item->fullId());
			session->data().requestItemViewRefresh(item);
		} else {
			result.push_back(item);
		}
	}
	return result;
}

void Restore(not_null<History*> history) {
	const auto session = &history->session();
	const auto peer = history->peer->id;
	if (!Enabled(session, peer)) {
		return;
	}
	auto &state = StateFor(session);
	auto &done = state.restoredBytes[peer];
	auto &restored = state.restored[peer];
	const auto self = session->userPeerId();
	const auto privateChat = history->peer->isUser();
	auto added = false;
	for (const auto &entry : MessageArchive::Load(session, peer)) {
		if (entry.kind != MessageArchive::Kind::Deleted
			|| done.contains(entry.message)) {
			continue;
		}
		// Still on screen: either it was kept when deleted, or it is not actually gone.
		if (entry.id.bare && session->data().message(peer, entry.id)) {
			continue;
		}
		const auto parsed = MessageArchive::Parse(entry.message);
		if (!parsed) {
			continue;
		}
		done.emplace(entry.message);
		const auto &data = parsed->c_message();
		const auto from = data.vfrom_id()
			? peerFromMTP(*data.vfrom_id())
			: PeerId();
		const auto out = privateChat ? (from == self) : data.is_out();
		const auto item = history->createItem(
			session->data().nextLocalMessageId(),
			ForChat(data, out),
			MessageFlags());
		state.marked.emplace(item->fullId());
		restored.push_back(item->fullId());
		added = true;
	}
	if (added) {
		history->checkLocalMessages();
	}
}

bool IsMarked(not_null<const HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto i = States.find(session);
	return (i != end(States)) && i->second->marked.contains(item->fullId());
}

QString Label() {
	return Tr(Str::DeletedLabel);
}

} // namespace Svipe::DeletedInChat
