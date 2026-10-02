/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The ledger of which Telegram account sat on which phone number, and when (Android:
SvipeNumberHistory + SvipeOldProfiles + SvipePhoneResolve).

A number is not an identity: an account moves to a new number and the old one carries somebody else
tomorrow, and Telegram only ever shows the present. So every number we are shown is written down as
it passes — "who has been on this number" and "which numbers has this account had" — for the owner
of this device. Only numbers Telegram shows us (contacts, as their privacy allows), only from the
moment this build runs; the server sync (svipe_number_sync) fills in what other Svipe devices saw.

Global, like Android's: a Telegram user id names the same person in every account.
*/
#pragma once

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::NumberHistory {

struct Account {
	uint64 userId = 0;
	QString name;
	QString username;
	int64 firstSeen = 0; // ms
	int64 lastSeen = 0; // ms
};

struct Number {
	QString phone; // digits only
	int64 firstSeen = 0; // ms
	int64 lastSeen = 0; // ms
};

[[nodiscard]] QString Normalize(const QString &phone);

// A user's number as Telegram shows it now (UserData::setPhone).
void Observe(not_null<UserData*> user, const QString &phone);

// A pairing somebody else observed, with the times the pool agreed on. Windows only widen.
bool Merge(uint64 userId, const QString &phone, int64 firstSeen, int64 lastSeen);

[[nodiscard]] std::vector<Account> AccountsOnNumber(const QString &phone);
[[nodiscard]] std::vector<Number> NumbersOfAccount(uint64 userId);

// Ids the server says were on this person's current number before them — ids only, never a number.
bool StoreOldProfiles(uint64 userId, const std::vector<uint64> &ids);
[[nodiscard]] std::vector<uint64> OldProfiles(uint64 userId);

// Who holds a number on Telegram right now, asked of Telegram itself at most once ever per number
// (contacts.resolvePhone is rate-limited): > 0 a user id, 0 nobody, -1 never asked.
[[nodiscard]] int64 Resolved(const QString &phone);
void Resolve(not_null<Main::Session*> session, const QString &phone);

// Fires whenever the ledger changes.
[[nodiscard]] rpl::producer<> Updates();

} // namespace Svipe::NumberHistory
