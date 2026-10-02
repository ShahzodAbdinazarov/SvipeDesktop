/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Pools the number ledger across Svipe apps (Android: SvipeNumberSync + SvipeNumberHistoryActivity),
over the same /v1/numbers endpoints. Opening a profile contributes what this device saw about that
person (when sharing is on) and merges the pooled PAST back into the local ledger. The server never
sends the number anybody is on today — only the numbers an account left behind, and the bare ids of
accounts that were on its current number before it; whether those people are visible at all stays
Telegram's answer, asked with this account's own credentials.
*/
#pragma once

class UserData;

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Svipe::NumberSync {

// Contribute and fetch for one profile; throttled, never blocks.
void SyncProfile(not_null<UserData*> user);

// The "Number history" block of Settings → Privacy and Security.
void BuildSettings(::Settings::Builder::SectionBuilder &builder);

} // namespace Svipe::NumberSync
