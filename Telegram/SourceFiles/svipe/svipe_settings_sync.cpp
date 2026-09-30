/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_settings_sync.h"

#include "base/call_delayed.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_bot_mute.h"
#include "svipe/svipe_message_types.h"
#include "svipe/svipe_storage.h"

#include <QtCore/QJsonArray>

namespace Svipe::SettingsSync {
namespace {

// Coalesce a burst of toggles into one write — flipping five switches is one intent.
constexpr auto kPushDebounce = crl::time(1500);

const auto kPath = u"/v1/settings/notifications"_q;
// The bucket as the server last gave it, so any part this client does not know goes back untouched.
const auto kRemoteValue = u"svipe_sync_remote_value"_q;

// The bucket carries several rules; the latest of them is what the bucket is dated by.
qint64 LocalUpdatedAt(not_null<Main::Session*> session) {
	return std::max(
		MessageTypes::UpdatedAt(session),
		BotMute::UpdatedAt(session));
}

struct State {
	bool pulled = false;
	bool pulling = false;
	uint64 pushGeneration = 0;
};

base::flat_map<uint64, State> &States() {
	static auto result = base::flat_map<uint64, State>();
	return result;
}

State &StateFor(not_null<Main::Session*> session) {
	return States()[session->userId().bare];
}

// The list names in the bucket and the storage prefixes they map to, kind by kind.
std::vector<std::pair<QString, QString>> Lists() {
	auto result = std::vector<std::pair<QString, QString>>();
	for (const auto &kind : MessageTypes::Kinds()) {
		result.emplace_back(u"muted_"_q + kind, MessageTypes::PrefixOf(true, kind));
		result.emplace_back(u"notified_"_q + kind, MessageTypes::PrefixOf(false, kind));
	}
	return result;
}

void PushNow(not_null<Main::Session*> session) {
	auto value = Storage::Get(session, kRemoteValue).toObject();
	value.insert(u"bots_muted"_q, BotMute::IsEnabled(session));
	auto exceptions = QJsonArray();
	for (const auto id : BotMute::Exceptions(session)) {
		exceptions.push_back(double(id));
	}
	value.insert(u"bot_exceptions"_q, exceptions);
	for (const auto &[name, prefix] : Lists()) {
		value.insert(
			name,
			QJsonArray::fromStringList(MessageTypes::TargetsOf(session, prefix)));
	}
	auto body = QJsonObject();
	body.insert(u"value"_q, value);
	body.insert(
		u"client_updated_at"_q,
		double(LocalUpdatedAt(session)));
	Auth::EnsureToken(session, crl::guard(session, [=](QString token) {
		if (token.isEmpty()) {
			return; // Nothing is lost: the next change or start pushes again.
		}
		Api::Put(kPath, body, token, crl::guard(session, [=](
				QJsonObject result,
				int code) {
			if (code >= 200 && code < 300) {
				Storage::Set(session, kRemoteValue, value);
			}
		}));
	}));
}

void Adopt(
		not_null<Main::Session*> session,
		const QJsonObject &value,
		qint64 remoteAt) {
	auto exceptions = std::vector<uint64>();
	for (const auto &id : value.value(u"bot_exceptions"_q).toArray()) {
		exceptions.push_back(uint64(id.toDouble()));
	}
	BotMute::Adopt(
		session,
		value.value(u"bots_muted"_q).toBool(),
		exceptions,
		remoteAt);
	for (const auto &[name, prefix] : Lists()) {
		// A device that predates a list sends no such key — "not mentioned" is not "none".
		if (!value.contains(name)) {
			continue;
		}
		auto targets = QStringList();
		for (const auto &id : value.value(name).toArray()) {
			targets.push_back(id.isString()
				? id.toString()
				: QString::number(qint64(id.toDouble())));
		}
		MessageTypes::AdoptList(session, prefix, targets);
	}
	MessageTypes::SetUpdatedAt(session, remoteAt);
}

void PullThen(not_null<Main::Session*> session, Fn<void()> then) {
	auto &state = StateFor(session);
	if (state.pulling) {
		return;
	}
	state.pulling = true;
	Auth::EnsureToken(session, crl::guard(session, [=](QString token) {
		if (token.isEmpty()) {
			StateFor(session).pulling = false;
			return;
		}
		Api::Get(kPath, token, crl::guard(session, [=](
				QJsonObject result,
				int code) {
			auto &state = StateFor(session);
			state.pulling = false;
			if (code < 200 || code >= 300) {
				return;
			}
			state.pulled = true;
			const auto remoteAt = qint64(
				result.value(u"client_updated_at"_q).toDouble());
			const auto value = result.value(u"value"_q).toObject();
			const auto localAt = LocalUpdatedAt(session);
			Storage::Set(session, kRemoteValue, value);
			if (value.isEmpty() || remoteAt <= 0) {
				// Nothing stored yet. If we hold a rule, this device is the one that knows.
				if (localAt > 0) {
					PushNow(session);
				}
			} else if (remoteAt < localAt) {
				PushNow(session); // Ours is the later intent — make sure the server has it.
			} else if (remoteAt > localAt) {
				Adopt(session, value, remoteAt);
			}
			if (then) {
				then();
			}
		}));
	}));
}

} // namespace

void Pull(not_null<Main::Session*> session) {
	PullThen(session, nullptr);
}

void Push(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	const auto generation = ++state.pushGeneration;
	base::call_delayed(kPushDebounce, session, [=] {
		auto &state = StateFor(session);
		if (state.pushGeneration != generation) {
			return; // A later toggle will push.
		}
		if (state.pulled) {
			PushNow(session);
		} else {
			// Never write the bucket unread: it carries rules this client does not own.
			PullThen(session, [=] { PushNow(session); });
		}
	});
}

} // namespace Svipe::SettingsSync
