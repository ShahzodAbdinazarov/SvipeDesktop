/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Profile photos kept after their owner deletes them (Android: SvipeAvatarStore + SvipeAvatarKeeper +
the deletion half of SvipeProfileImages).

Every profile-photo list tdesktop fetches (photos.getUserPhotos, Api::PeerPhoto::requestUserPhotos)
passes through Remember(): each photo is recorded in a small per-person ledger and its largest size is
copied to disk, so a photo the person later removes can still be shown. Like Android, only photos
seen online are captured — Telegram never delivers a photo that was set and removed while we were
not looking. Deletion is not stored: it is derived at display time as captured − live, and only
trusted when the live list is complete (every page loaded).

The ledger is global, not per account: a Telegram user id names the same person in every account.
*/
#pragma once

class PhotoData;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::AvatarArchive {

struct Photo {
	PhotoId id = 0;
	TimeId date = 0; // when the person set it
	TimeId capturedAt = 0; // when we first saw it
};

// A page of a person's live profile photos, as requestUserPhotos received it.
void Remember(
	not_null<UserData*> user,
	const std::vector<not_null<PhotoData*>> &photos,
	int fullCount,
	bool firstPage);

// Add photos that arrived from the server archive (bytes already on disk at FilePath()).
void RecordSynced(UserId user, PhotoId photo, TimeId date);

[[nodiscard]] std::vector<Photo> Captured(UserId user);
[[nodiscard]] QString FilePath(UserId user, PhotoId photo);
[[nodiscard]] bool HasFile(UserId user, PhotoId photo);

struct LiveSet {
	std::vector<PhotoId> ids;
	bool complete = false; // every page arrived: a captured id missing here really was deleted
};
[[nodiscard]] LiveSet Live(not_null<UserData*> user);

// Captured photos that are no longer live, newest set first. Empty unless the live set is complete.
[[nodiscard]] std::vector<Photo> Deleted(not_null<UserData*> user);

// Fires with the person whose captured set, files or live set changed.
[[nodiscard]] rpl::producer<UserId> Updates();

} // namespace Svipe::AvatarArchive
