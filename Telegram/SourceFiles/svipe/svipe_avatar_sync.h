/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The shared archive of deleted profile photos (Android: SvipeAvatarSync + SvipeAvatarSettingsActivity),
over the same /v1/avatars endpoints.

Every profile view reports which photo ids are live (the proof the server later checks before showing
anyone this person's archive) and which captured ids have gone; the server answers with the deleted
ones it does not hold, and only those are uploaded, straight to storage through a presigned URL. The
same view pulls the person's archive back, so photos other Svipe users kept appear here too. A
deletion is only reported when the live list is complete — a missing id in a partial list means
"not loaded yet", not "deleted".
*/
#pragma once

class UserData;

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Svipe::AvatarSync {

// After a page of a person's profile photos arrived (and AvatarArchive recorded it).
void OnProfileSeen(not_null<UserData*> user);

// The "Profile photo archive" block of Settings → Privacy and Security.
void BuildSettings(::Settings::Builder::SectionBuilder &builder);

} // namespace Svipe::AvatarSync
