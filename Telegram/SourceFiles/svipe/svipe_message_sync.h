/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The server sync of the Recent actions archive (Android: SvipeMessageSync, SvipeMsgSyncPrompt,
SvipeMessageSyncSettingsActivity). Same endpoints, same consent flow, and the same keys: an item is
content-addressed by sha256(author:date:sha256(text\0mediaId)), computed identically on both sides of
a chat, so the phone, this app and the other person's device all collapse one deletion into one row.

The sync is also what makes the desktop archive complete: deletions that happened while this app was
closed were never seen here, but the phone saw them and uploaded them.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

class History;

namespace Svipe::MessageSync {

inline constexpr auto kWithPartner = "with_partner";
inline constexpr auto kSelfOnly = "self_only";
inline constexpr auto kOff = "off";

// Load the mode from the server and offer the consent prompt after a capture in the open 1:1 chat.
void Start(not_null<Main::Session*> session);

// Upload this 1:1 chat's archive per the mode, then pull what the server holds for it.
void OnChatOpened(not_null<History*> history);

// The "Deleted message sync" block of Settings → Privacy and Security.
void BuildSettings(::Settings::Builder::SectionBuilder &builder);

// Exposed for the check against the Android implementation.
[[nodiscard]] QString MergeKey(
	int64 authorId,
	TimeId date,
	const QString &text,
	int64 mediaId);

} // namespace Svipe::MessageSync
