/*
Svipe Desktop — Svipe additions to Telegram Desktop.

"Show in chat" (Android: SvipeConfig.isShowInChat + the processDeletedMessages keep-branch, the
getMessagesInternal merge and the red "Deleted" in ChatMessageCell). Per chat, off by default.

When it is on:
- a message the other side deletes while the chat is loaded is not removed, only marked;
- the chat's archived deletions — captured here earlier or synced from the phone — come back as
  client-side items, which tdesktop already places by date into whatever range of the history is
  loaded (History::checkLocalMessages), slice after slice, as Android merges them into each load;
- both carry a red "Deleted" before the time.

My own deletions are not kept: deleting a message myself is meant to remove it from my screen.
*/
#pragma once

class History;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::DeletedInChat {

// Restore again whenever the chat's archive changes (a sync brought older deletions in).
void Start(not_null<Main::Session*> session);

[[nodiscard]] bool Enabled(not_null<Main::Session*> session, PeerId peer);
[[nodiscard]] rpl::producer<bool> EnabledValue(
	not_null<Main::Session*> session,
	PeerId peer);
void SetEnabled(not_null<History*> history, bool enabled);

// Server deletions: the ones in a chat with the option on are marked and kept; the rest are returned
// to be destroyed as usual.
[[nodiscard]] std::vector<not_null<HistoryItem*>> KeepOrDestroy(
	std::vector<not_null<HistoryItem*>> items);

// Bring the chat's archived deletions back into it (no-op when the option is off).
void Restore(not_null<History*> history);

[[nodiscard]] bool IsMarked(not_null<const HistoryItem*> item);
[[nodiscard]] QString Label();

} // namespace Svipe::DeletedInChat
