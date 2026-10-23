/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Voice and round-video messages as text through Svipe's own server, for everybody (the Android app's
SvipeStt.java). While the server says a provider is configured (GET /v1/stt/status) the transcribe
button shows "→S" for every account and Api::Transcribes sends the voice note to POST /v1/stt instead
of messages.transcribeAudio. Holding the button down swaps to Telegram's "→A" for the whole app.
*/
#pragma once

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Svipe::Stt {

// The server has a provider. Asks it once per run, in the background.
[[nodiscard]] bool Available(not_null<Main::Session*> session);
// Our engine answers the button right now: available and nobody switched to "A".
[[nodiscard]] bool Active(not_null<Main::Session*> session);

// Swap S and A for the whole app. Returns true when Telegram's engine is now the one in use.
bool ToggleEngine();
[[nodiscard]] rpl::producer<> EngineChanged();

// The text of the item's voice note, or nullopt when it could not be had.
void Transcribe(
	not_null<HistoryItem*> item,
	Fn<void(std::optional<QString>)> done);

} // namespace Svipe::Stt
