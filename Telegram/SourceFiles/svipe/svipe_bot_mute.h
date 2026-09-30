/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Bots as their own notification category, ported one to one from the Android app (SvipeBotMute.java).

Telegram files a bot under "Private chats", so the only way to silence bots is to silence every
private chat. This gives bots their own switch, with exceptions. It is enforced with Telegram's OWN
per-peer mute, so a bot muted here is muted for the server and in every other client too; what Svipe
owns is the RULE ("bots are muted, except these") and the memory of which peers the rule muted, so
switching it off releases exactly those and never a bot the user muted by hand. The rule travels
between Svipe installs in the same /v1/settings/notifications bucket as the message-type lists.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Svipe::BotMute {

[[nodiscard]] bool IsEnabled(not_null<Main::Session*> session);
void SetEnabled(not_null<Main::Session*> session, bool enabled);

[[nodiscard]] std::vector<uint64> Exceptions(not_null<Main::Session*> session);
void SetException(not_null<Main::Session*> session, uint64 userId, bool except);

[[nodiscard]] qint64 UpdatedAt(not_null<Main::Session*> session);

// A rule that arrived from another device: stored and applied, never pushed back.
void Adopt(
	not_null<Main::Session*> session,
	bool enabled,
	const std::vector<uint64> &exceptions,
	qint64 updatedAt);

// Keep the rule true while the session lives: new bot chats, and Telegram rewriting its own
// defaults (which drops a per-peer mute it considers redundant).
void Watch(not_null<Main::Session*> session);

// Fires when the rule or its exceptions change, here or from another device.
[[nodiscard]] rpl::producer<> Changes();

} // namespace Svipe::BotMute
