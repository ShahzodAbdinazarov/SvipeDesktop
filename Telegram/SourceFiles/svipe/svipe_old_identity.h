/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The two profile tabs built from the number ledger (Android: SvipeOldIdentity +
SvipeOldIdentityAdapter in SharedMediaLayout):

  Old profiles — accounts that sat on THIS person's number before them; each row says where that
  account went (its number now, if Telegram shows it) and opens it when Telegram still lets us.
  Old numbers — numbers THIS account used before its current one; each row says who holds that
  number now, and opens them when we can.

An account we cannot see is drawn as Telegram's own "Deleted Account" and is not openable: we know a
number changed hands, not to whom.
*/
#pragma once

#include "info/profile/tabs/info_profile_tab_content.h"

class UserData;

namespace Svipe::OldIdentity {

[[nodiscard]] Info::Profile::MediaTabDescriptor MakeOldProfilesTab(
	not_null<UserData*> user);
[[nodiscard]] Info::Profile::MediaTabDescriptor MakeOldNumbersTab(
	not_null<UserData*> user);

} // namespace Svipe::OldIdentity
