/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Carries the settings the client owns to the user's other Svipe installs, through the backend bucket
/v1/settings/notifications — the one the Android app uses (SvipeSettingsSync.java). The desktop owns
only the message-type lists in it; everything else in the bucket (the Android bot-mute rule) is sent
back exactly as it came, and nothing is sent before the bucket has been read once.

Conflicts are settled by when the user made the change (client_updated_at), not by which device
happened to connect first.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Svipe::SettingsSync {

// Read what the other devices know and adopt it if it is newer. Called when the session starts.
void Pull(not_null<Main::Session*> session);

// Send the local lists up, debounced. Safe to call on every toggle.
void Push(not_null<Main::Session*> session);

} // namespace Svipe::SettingsSync
