/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_search_log.h"

#include "base/timer.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUuid>

namespace Svipe::SearchLog {
namespace {

// Android: the 500 ms debounce in DialogsActivity.svipeLogNativeQuery.
constexpr auto kSettle = crl::time(500);
constexpr auto kMaxQueries = 60; // the server's cap

struct Visit {
	QString sessionId;
	QStringList queries;
	QString lastQuery;
};

struct State {
	std::optional<Visit> visit;
	QString pending;
	base::Timer timer;
	base::weak_ptr<Main::Session> session;
};

State &Current() {
	static auto result = State();
	return result;
}

// SvipeSearchLog.collapse: a prefix-extension (or a backspace) replaces the last variant, a
// different query appends, an exact repeat is a no-op.
void Collapse(QStringList &queries, const QString &q) {
	if (q.isEmpty() || (!queries.isEmpty() && queries.back() == q)) {
		return;
	}
	const auto &last = queries.isEmpty() ? QString() : queries.back();
	if (!last.isEmpty() && (q.startsWith(last) || last.startsWith(q))) {
		queries.back() = q;
	} else {
		queries.push_back(q);
		while (queries.size() > kMaxQueries) {
			queries.pop_front();
		}
	}
}

void Send(not_null<Main::Session*> session, QJsonObject body) {
	// Fire and forget: telemetry never touches the UX.
	Auth::EnsureToken(session, [=](QString token) {
		if (!token.isEmpty()) {
			Api::Post(u"/v1/search/session"_q, body, token, nullptr);
		}
	});
}

[[nodiscard]] QJsonObject Body(const Visit &visit) {
	auto body = QJsonObject{
		{ u"session_id"_q, visit.sessionId },
		{ u"source"_q, u"chats"_q },
	};
	if (!visit.queries.isEmpty()) {
		body.insert(u"queries"_q, QJsonArray::fromStringList(visit.queries));
	}
	return body;
}

void Settle() {
	auto &state = Current();
	const auto session = state.session.get();
	const auto q = state.pending;
	if (!session || q.isEmpty()) {
		return;
	}
	if (!state.visit) {
		state.visit = Visit{
			.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces),
		};
	}
	state.visit->lastQuery = q;
	Collapse(state.visit->queries, q);
	Send(session, Body(*state.visit)); // the native search reports no result count
}

} // namespace

void Query(not_null<Main::Session*> session, const QString &text) {
	auto &state = Current();
	const auto q = text.trimmed();
	if (state.session.get() != session) {
		state.visit = std::nullopt;
		state.session = base::make_weak(session);
	}
	state.pending = q;
	if (q.isEmpty()) {
		state.timer.cancel();
		state.visit = std::nullopt; // field emptied: this search visit is over
		return;
	}
	state.timer.setCallback(Settle);
	state.timer.callOnce(kSettle); // restarts: only a query that stays put is recorded
}

void Click(not_null<PeerData*> peer) {
	auto &state = Current();
	if (state.timer.isActive()) {
		state.timer.cancel();
		Settle(); // the query that found it counts even when clicked within the debounce
	}
	if (!state.visit || state.session.get() != &peer->session()) {
		return;
	}
	auto kind = QString();
	auto ref = QString();
	auto label = QString();
	const auto handle = peer->username();
	if (peer->isUser()) {
		kind = u"user"_q;
		ref = handle.isEmpty() ? QString::number(peerToUser(peer->id).bare) : handle;
	} else {
		// A broadcast channel is what the catalogue cares about; a supergroup is not one.
		const auto broadcast = peer->isBroadcast();
		kind = broadcast ? u"channel"_q : u"group"_q;
		if (broadcast && !handle.isEmpty()) {
			ref = handle;
			label = peer->name(); // public channel: catalogue data, kept in full
		} else {
			ref = QString::number(peer->isChannel()
				? peerToChannel(peer->id).bare
				: peerToChat(peer->id).bare); // the id alone, nothing else
		}
	}
	auto clicked = QJsonObject{
		{ u"kind"_q, kind },
		{ u"ref"_q, ref },
	};
	if (!state.visit->lastQuery.isEmpty()) {
		clicked.insert(u"query"_q, state.visit->lastQuery);
	}
	if (!label.isEmpty()) {
		clicked.insert(u"label"_q, label);
	}
	auto body = Body(*state.visit);
	body.insert(u"clicked"_q, clicked);
	Send(&peer->session(), body);
}

} // namespace Svipe::SearchLog
