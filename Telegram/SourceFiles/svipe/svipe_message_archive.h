/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The archive of deleted and edited messages behind each chat's "Recent actions" (Android:
SvipeMessageArchiveStore + the three MessagesStorage capture hooks).

Android can archive anything it ever stored, because every message lands in its local database.
tdesktop keeps messages only in memory, built from the MTP objects the server sends, and drops those
objects once the HistoryItem exists. So the archive keeps a copy of the MTP bytes of every regular
message as it arrives (Remember), and when one is deleted or edited it moves that copy into a
per-chat file. A message deleted while the app was closed is never seen here; the server sync fills
that gap from the phone.

The stored bytes are the TL serialization of `message` — the same bytes Android uploads, both at
layer 229 — so an entry written here, one written on the phone and one pulled from the server are
interchangeable.
*/
#pragma once

class History;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::MessageArchive {

enum class Kind : int {
	Deleted = 0, // a message that was deleted
	EditedPrior = 1, // a pre-edit version of a still-live message
};

struct Entry {
	Kind kind = Kind::Deleted;
	TimeId date = 0; // the message's own date
	TimeId editDate = 0; // the message's edit date at the time it was archived
	TimeId archivedAt = 0; // when this device archived it (or 0 for a synced entry)
	MsgId id = 0; // the message id on this account (0 for a synced entry from the other side)
	QString mergeKey; // the server sync key, empty until the entry is synced
	QByteArray message; // TL-serialized `message`
};

// Keep the MTP bytes of a message that has just been created from the server.
void Remember(not_null<History*> history, MsgId id, const MTPMessage &message);

// The server confirmed a message this account sent: keep its full copy (when the update carries one)
// or, for a private-chat text sent through updateShortSentMessage, a copy rebuilt from the item.
void RememberExisting(not_null<Main::Session*> session, const MTPMessage &message);
void RememberSent(not_null<HistoryItem*> item);

// Move the remembered copy into the archive before the item goes away.
void CaptureDeleted(not_null<HistoryItem*> item);
void CaptureDeleted(const std::vector<not_null<HistoryItem*>> &items);

// Archive the remembered version when an edit arrives with a newer edit date, then remember the new one.
void CaptureEdited(not_null<HistoryItem*> item, const MTPMessage &edited);

// Add entries that arrived from the server sync; ones already present (same merge key) are skipped.
void AddSynced(
	not_null<Main::Session*> session,
	PeerId peer,
	std::vector<Entry> entries);

// Remember that an entry of this chat has been synced under that key.
void SetMergeKey(
	not_null<Main::Session*> session,
	PeerId peer,
	const QByteArray &message,
	const QString &mergeKey);

[[nodiscard]] std::vector<Entry> Load(
	not_null<Main::Session*> session,
	PeerId peer);
[[nodiscard]] bool HasEntries(not_null<Main::Session*> session, PeerId peer);

// Fires with the chat whose archive changed.
[[nodiscard]] rpl::producer<PeerId> Updates(not_null<Main::Session*> session);

[[nodiscard]] std::optional<MTPMessage> Parse(const QByteArray &bytes);

// The remembered MTP bytes of a live message (the newest version an edit chain ends at), if any.
[[nodiscard]] QByteArray LiveBytes(not_null<HistoryItem*> item);

} // namespace Svipe::MessageArchive
