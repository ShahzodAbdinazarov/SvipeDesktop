/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bot_mute.h"

#include "base/call_delayed.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "svipe/svipe_settings_sync.h"
#include "svipe/svipe_storage.h"

#include <QtCore/QJsonArray>

namespace Svipe::BotMute {
namespace {

// The Android client's pref names, so the two read as one design.
const auto kEnabled = u"svipe_bot_mute"_q;
const auto kExceptions = u"svipe_bot_mute_exceptions"_q;
const auto kApplied = u"svipe_bot_mute_applied"_q;
const auto kUpdated = u"svipe_bot_mute_updated"_q;

constexpr auto kReapplyDelay = crl::time(500);

rpl::event_stream<> &ChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

std::vector<uint64> ReadIds(
		not_null<Main::Session*> session,
		const QString &key) {
	auto result = std::vector<uint64>();
	for (const auto &value : Storage::Get(session, key).toArray()) {
		result.push_back(uint64(value.toDouble()));
	}
	return result;
}

void WriteIds(
		not_null<Main::Session*> session,
		const QString &key,
		const std::vector<uint64> &ids) {
	auto array = QJsonArray();
	for (const auto id : ids) {
		array.push_back(double(id));
	}
	Storage::Set(session, key, array);
}

[[nodiscard]] bool Contains(const std::vector<uint64> &ids, uint64 id) {
	return ranges::contains(ids, id);
}

void Touch(not_null<Main::Session*> session) {
	Storage::Set(session, kUpdated, double(QDateTime::currentMSecsSinceEpoch()));
}

void Mute(not_null<History*> history, bool mute) {
	history->owner().notifySettings().update(
		history,
		mute ? Data::MuteValue{ .forever = true } : Data::MuteValue{ .unmute = true });
}

template <typename Callback>
void ForEachBotHistory(not_null<Main::Session*> session, Callback &&callback) {
	const auto walk = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			const auto history = row->history();
			const auto user = history ? history->peer->asUser() : nullptr;
			if (user && user->isBot()) {
				callback(not_null(history));
			}
		}
	};
	walk(session->data().chatsList());
	if (const auto archive = session->data().folderLoaded(Data::Folder::kId)) {
		walk(archive->chatsList());
	}
}

// Bring every bot chat in line with the rule — and only the ones the rule owns. Reconciled against
// what IS muted, not against what we remember doing: Telegram drops a per-peer mute it considers
// redundant, so a rule that trusted its bookkeeping would read "muted" while every bot notified.
void ApplyAll(not_null<Main::Session*> session) {
	const auto on = IsEnabled(session);
	const auto except = Exceptions(session);
	auto applied = ReadIds(session, kApplied);
	const auto settings = &session->data().notifySettings();
	ForEachBotHistory(session, [&](not_null<History*> history) {
		const auto userId = peerToUser(history->peer->id).bare;
		const auto shouldMute = on && !Contains(except, userId);
		const auto currentlyMuted = settings->isMuted(history);
		if (shouldMute) {
			if (!currentlyMuted) {
				Mute(history, true);
			}
			if (!Contains(applied, userId)) {
				applied.push_back(userId);
			}
		} else if (Contains(applied, userId)) {
			applied.erase(ranges::remove(applied, userId), end(applied));
			if (currentlyMuted) {
				Mute(history, false);
			}
		}
	});
	WriteIds(session, kApplied, applied);
}

void ApplyTo(not_null<Main::Session*> session, uint64 userId, bool mute) {
	auto applied = ReadIds(session, kApplied);
	const auto user = session->data().userLoaded(UserId(userId));
	const auto history = user ? session->data().historyLoaded(user->id) : nullptr;
	if (mute) {
		if (!Contains(applied, userId)) {
			applied.push_back(userId);
			if (history) {
				Mute(history, true);
			}
		}
	} else if (Contains(applied, userId)) {
		applied.erase(ranges::remove(applied, userId), end(applied));
		if (history) {
			Mute(history, false);
		}
	}
	WriteIds(session, kApplied, applied);
}

base::flat_set<uint64> &Watched() {
	static auto result = base::flat_set<uint64>();
	return result;
}

} // namespace

bool IsEnabled(not_null<Main::Session*> session) {
	return Storage::Get(session, kEnabled).toBool();
}

void SetEnabled(not_null<Main::Session*> session, bool enabled) {
	Storage::Set(session, kEnabled, enabled);
	Touch(session);
	ApplyAll(session);
	ChangesStream().fire({});
	SettingsSync::Push(session);
}

std::vector<uint64> Exceptions(not_null<Main::Session*> session) {
	return ReadIds(session, kExceptions);
}

void SetException(not_null<Main::Session*> session, uint64 userId, bool except) {
	auto ids = Exceptions(session);
	if (except == Contains(ids, userId)) {
		return;
	}
	if (except) {
		ids.push_back(userId);
	} else {
		ids.erase(ranges::remove(ids, userId), end(ids));
	}
	WriteIds(session, kExceptions, ids);
	Touch(session);
	// Apply to the one bot that changed rather than re-walking every chat.
	if (IsEnabled(session)) {
		ApplyTo(session, userId, !except);
	} else if (!except) {
		// Rule is off: an exception means nothing, but leaving a mute we applied would.
		ApplyTo(session, userId, false);
	}
	ChangesStream().fire({});
	SettingsSync::Push(session);
}

qint64 UpdatedAt(not_null<Main::Session*> session) {
	return qint64(Storage::Get(session, kUpdated).toDouble());
}

void Adopt(
		not_null<Main::Session*> session,
		bool enabled,
		const std::vector<uint64> &exceptions,
		qint64 updatedAt) {
	Storage::Set(session, kEnabled, enabled);
	Storage::Set(session, kUpdated, double(updatedAt));
	WriteIds(session, kExceptions, exceptions);
	ApplyAll(session);
	ChangesStream().fire({});
}

void Watch(not_null<Main::Session*> session) {
	if (!Watched().emplace(session->userId().bare).second) {
		return;
	}
	// Debounced: the chat list changes in bursts, and one pass after the burst is enough.
	const auto generation = std::make_shared<uint64>(0);
	const auto reapply = [=] {
		const auto mine = ++*generation;
		base::call_delayed(kReapplyDelay, session, [=] {
			if (*generation == mine && IsEnabled(session)) {
				ApplyAll(session);
			}
		});
	};
	// New bot chats obey a rule set before they existed.
	session->data().chatsListChanges(
	) | rpl::on_next(reapply, session->lifetime());
	// Telegram rewrote its own defaults: ours has to be re-asserted on top.
	session->data().notifySettings().defaultUpdates(
		Data::DefaultNotify::User
	) | rpl::on_next([=] {
		if (IsEnabled(session)) {
			Storage::Remove(session, kApplied);
		}
		reapply();
	}, session->lifetime());
	reapply();
}

rpl::producer<> Changes() {
	return ChangesStream().events();
}

} // namespace Svipe::BotMute
