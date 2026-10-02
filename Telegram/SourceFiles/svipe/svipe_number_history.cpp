/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_number_history.h"

#include "apiwrap.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>

namespace Svipe::NumberHistory {
namespace {

// Android: SvipeNumberHistory.MAX_PER_KEY / SvipeOldProfiles.MAX_PER_USER.
constexpr auto kMaxPerKey = 40;
constexpr auto kSaveDelayMs = 2000;

struct State {
	bool loaded = false;
	bool saveScheduled = false;
	std::map<QString, std::vector<Account>> byPhone;
	std::map<uint64, std::vector<Number>> byUser;
	std::map<uint64, std::vector<uint64>> oldProfiles;
	std::map<QString, int64> resolved; // user id, or 0 for nobody
	int64 floodUntil = 0; // ms
	base::flat_map<uint64, QString> lastKnown;
	base::flat_set<QString> inFlight;
	rpl::event_stream<> updates;
};

State &GlobalState() {
	static auto result = State();
	return result;
}

[[nodiscard]] int64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] QString Path() {
	return cWorkingDir() + u"tdata/svipe/numbers.json"_q;
}

// Ids and times as strings: a JSON double would round a 64-bit value.
[[nodiscard]] QString S(int64 value) {
	return QString::number(value);
}
[[nodiscard]] int64 I(const QJsonValue &value) {
	return value.isString()
		? value.toString().toLongLong()
		: int64(value.toDouble());
}

void Load() {
	auto &state = GlobalState();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto root = QJsonDocument::fromJson(file.readAll()).object();
	const auto byPhone = root.value(u"p"_q).toObject();
	for (auto i = byPhone.begin(); i != byPhone.end(); ++i) {
		auto &list = state.byPhone[i.key()];
		for (const auto &value : i.value().toArray()) {
			const auto o = value.toObject();
			list.push_back({
				.userId = uint64(I(o.value(u"uid"_q))),
				.name = o.value(u"name"_q).toString(),
				.username = o.value(u"uname"_q).toString(),
				.firstSeen = I(o.value(u"first"_q)),
				.lastSeen = I(o.value(u"last"_q)),
			});
		}
	}
	const auto byUser = root.value(u"u"_q).toObject();
	for (auto i = byUser.begin(); i != byUser.end(); ++i) {
		auto &list = state.byUser[i.key().toULongLong()];
		for (const auto &value : i.value().toArray()) {
			const auto o = value.toObject();
			list.push_back({
				.phone = o.value(u"phone"_q).toString(),
				.firstSeen = I(o.value(u"first"_q)),
				.lastSeen = I(o.value(u"last"_q)),
			});
		}
	}
	const auto old = root.value(u"o"_q).toObject();
	for (auto i = old.begin(); i != old.end(); ++i) {
		auto &list = state.oldProfiles[i.key().toULongLong()];
		for (const auto &value : i.value().toArray()) {
			list.push_back(uint64(I(value)));
		}
	}
	const auto resolved = root.value(u"r"_q).toObject();
	for (auto i = resolved.begin(); i != resolved.end(); ++i) {
		state.resolved[i.key()] = I(i.value());
	}
	state.floodUntil = I(root.value(u"flood"_q));
	// "Already recorded", so a restart does not rewrite the ledger: each account's latest number.
	for (const auto &[userId, numbers] : state.byUser) {
		const auto latest = ranges::max_element(numbers, ranges::less(), &Number::firstSeen);
		if (latest != end(numbers)) {
			state.lastKnown[userId] = latest->phone;
		}
	}
}

void SaveNow() {
	auto &state = GlobalState();
	state.saveScheduled = false;
	auto byPhone = QJsonObject();
	for (const auto &[phone, list] : state.byPhone) {
		auto array = QJsonArray();
		for (const auto &a : list) {
			auto o = QJsonObject{
				{ u"uid"_q, S(a.userId) },
				{ u"first"_q, S(a.firstSeen) },
				{ u"last"_q, S(a.lastSeen) },
			};
			if (!a.name.isEmpty()) o.insert(u"name"_q, a.name);
			if (!a.username.isEmpty()) o.insert(u"uname"_q, a.username);
			array.push_back(o);
		}
		byPhone.insert(phone, array);
	}
	auto byUser = QJsonObject();
	for (const auto &[userId, list] : state.byUser) {
		auto array = QJsonArray();
		for (const auto &n : list) {
			array.push_back(QJsonObject{
				{ u"phone"_q, n.phone },
				{ u"first"_q, S(n.firstSeen) },
				{ u"last"_q, S(n.lastSeen) },
			});
		}
		byUser.insert(S(userId), array);
	}
	auto old = QJsonObject();
	for (const auto &[userId, list] : state.oldProfiles) {
		auto array = QJsonArray();
		for (const auto id : list) {
			array.push_back(S(id));
		}
		old.insert(S(userId), array);
	}
	auto resolved = QJsonObject();
	for (const auto &[phone, id] : state.resolved) {
		resolved.insert(phone, S(id));
	}
	const auto root = QJsonObject{
		{ u"p"_q, byPhone },
		{ u"u"_q, byUser },
		{ u"o"_q, old },
		{ u"r"_q, resolved },
		{ u"flood"_q, S(state.floodUntil) },
	};
	QDir().mkpath(QFileInfo(Path()).absolutePath());
	auto file = QFile(Path());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}

// setPhone runs for nearly every user object, so writes are batched.
void Changed() {
	auto &state = GlobalState();
	state.updates.fire({});
	if (!state.saveScheduled) {
		state.saveScheduled = true;
		QTimer::singleShot(kSaveDelayMs, [] { SaveNow(); });
	}
}

template <typename T>
void Trim(std::vector<T> &list) {
	if (list.size() > kMaxPerKey) {
		list.erase(begin(list), begin(list) + (list.size() - kMaxPerKey));
	}
}

// Both indexes for one pairing. first/last of 0 mean "now"; a pooled pairing passes real times and
// the stored window only widens: the earliest first-seen and the latest last-seen win.
bool Record(
		uint64 userId,
		const QString &phone,
		const QString &name,
		const QString &username,
		int64 firstSeen,
		int64 lastSeen) {
	Load();
	auto &state = GlobalState();
	const auto stamp = NowMs();
	const auto first = firstSeen > 0 ? firstSeen : stamp;
	const auto last = lastSeen > 0 ? lastSeen : stamp;
	auto changed = false;

	auto &accounts = state.byPhone[phone];
	const auto a = ranges::find(accounts, userId, &Account::userId);
	if (a != end(accounts)) {
		if (last > a->lastSeen) {
			a->lastSeen = last;
			changed = true;
		}
		if (first < a->firstSeen) {
			a->firstSeen = first;
			changed = true;
		}
		if (!name.isEmpty()) a->name = name;
		if (!username.isEmpty()) a->username = username;
	} else {
		accounts.push_back({ userId, name, username, first, last });
		Trim(accounts);
		changed = true;
	}

	auto &numbers = state.byUser[userId];
	const auto n = ranges::find(numbers, phone, &Number::phone);
	if (n != end(numbers)) {
		if (last > n->lastSeen) {
			n->lastSeen = last;
			changed = true;
		}
		if (first < n->firstSeen) {
			n->firstSeen = first;
			changed = true;
		}
	} else {
		numbers.push_back({ phone, first, last });
		Trim(numbers);
		changed = true;
	}
	if (changed) {
		Changed();
	}
	return changed;
}

} // namespace

QString Normalize(const QString &phone) {
	auto result = QString();
	result.reserve(phone.size());
	for (const auto ch : phone) {
		if (ch >= '0' && ch <= '9') {
			result.append(ch);
		}
	}
	return result;
}

void Observe(not_null<UserData*> user, const QString &phone) {
	const auto digits = Normalize(phone);
	if (digits.isEmpty() || user->isInaccessible()) {
		return; // not a contact, or the number is not ours to see
	}
	Load();
	auto &state = GlobalState();
	const auto userId = peerToUser(user->id).bare;
	const auto i = state.lastKnown.find(userId);
	if (i != end(state.lastKnown) && i->second == digits) {
		return; // unchanged, the overwhelmingly common case
	}
	state.lastKnown[userId] = digits;
	Record(userId, digits, user->name(), user->username(), 0, 0);
}

bool Merge(uint64 userId, const QString &phone, int64 firstSeen, int64 lastSeen) {
	const auto digits = Normalize(phone);
	if (!userId || digits.isEmpty()) {
		return false;
	}
	// Never touches lastKnown: a number another device saw years ago is not this device's view now.
	return Record(userId, digits, QString(), QString(), firstSeen, lastSeen);
}

std::vector<Account> AccountsOnNumber(const QString &phone) {
	Load();
	const auto &byPhone = GlobalState().byPhone;
	const auto i = byPhone.find(Normalize(phone));
	auto result = (i != end(byPhone)) ? i->second : std::vector<Account>();
	ranges::sort(result, ranges::less(), &Account::firstSeen);
	return result;
}

std::vector<Number> NumbersOfAccount(uint64 userId) {
	Load();
	const auto &byUser = GlobalState().byUser;
	const auto i = byUser.find(userId);
	auto result = (i != end(byUser)) ? i->second : std::vector<Number>();
	ranges::sort(result, ranges::less(), &Number::firstSeen);
	return result;
}

bool StoreOldProfiles(uint64 userId, const std::vector<uint64> &ids) {
	Load();
	auto &list = GlobalState().oldProfiles[userId];
	auto changed = false;
	for (const auto id : ids) {
		if (id && id != userId && !ranges::contains(list, id)) {
			list.push_back(id);
			changed = true;
		}
	}
	if (changed) {
		Trim(list);
		Changed();
	}
	return changed;
}

std::vector<uint64> OldProfiles(uint64 userId) {
	Load();
	const auto &old = GlobalState().oldProfiles;
	const auto i = old.find(userId);
	return (i != end(old)) ? i->second : std::vector<uint64>();
}

int64 Resolved(const QString &phone) {
	Load();
	const auto &resolved = GlobalState().resolved;
	const auto i = resolved.find(Normalize(phone));
	return (i != end(resolved)) ? i->second : -1;
}

void Resolve(not_null<Main::Session*> session, const QString &phone) {
	const auto digits = Normalize(phone);
	auto &state = GlobalState();
	if (digits.isEmpty()
		|| Resolved(digits) != -1
		|| state.inFlight.contains(digits)
		|| state.floodUntil > NowMs()) {
		return;
	}
	state.inFlight.emplace(digits);
	session->api().request(MTPcontacts_ResolvePhone(
		MTP_string(digits)
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		auto &state = GlobalState();
		state.inFlight.remove(digits);
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = peerFromMTP(data.vpeer());
		state.resolved[digits] = peerIsUser(peer)
			? int64(peerToUser(peer).bare)
			: 0;
		Changed();
	}).fail([=](const MTP::Error &error) {
		auto &state = GlobalState();
		state.inFlight.remove(digits);
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			// Asking again inside the wait is what makes Telegram extend it: remember it across
			// launches (SvipePhoneResolve's flood discipline).
			const auto seconds = type.mid(u"FLOOD_WAIT_"_q.size()).toInt();
			state.floodUntil = NowMs() + int64(std::max(seconds, 60)) * 1000;
			Changed();
		} else if (error.code() == 400) {
			state.resolved[digits] = 0; // nobody holds it, or they can't be found by number
			Changed();
		}
	}).send();
}

rpl::producer<> Updates() {
	return GlobalState().updates.events();
}

} // namespace Svipe::NumberHistory
