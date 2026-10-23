/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_stt.h"

#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "base/call_delayed.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtWidgets/QApplication>

namespace Svipe::Stt {
namespace {

// How long to wait for a voice note that is not on this computer yet.
constexpr auto kDownloadTimeout = 60 * crl::time(1000);

enum class Status {
	Unknown,
	Asking,
	Yes,
	No,
};

Status GlobalStatus = Status::Unknown;

QString EnginePath() {
	return cWorkingDir() + u"tdata/svipe_stt_engine"_q;
}

bool &TelegramEngineValue() {
	static auto result = QFile::exists(EnginePath());
	return result;
}

rpl::event_stream<> &EngineChanges() {
	static auto result = rpl::event_stream<>();
	return result;
}

void Ask(not_null<Main::Session*> session) {
	GlobalStatus = Status::Asking;
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			GlobalStatus = Status::Unknown;
			return;
		}
		Api::Get(u"/v1/stt/status"_q, token, [=](QJsonObject result, int code) {
			if (code != 200) {
				GlobalStatus = Status::Unknown;
				return;
			}
			const auto was = GlobalStatus;
			GlobalStatus = result.value(u"enabled"_q).toBool()
				? Status::Yes
				: Status::No;
			if (was != GlobalStatus) {
				// Every voice message on screen re-decides its button.
				EngineChanges().fire({});
			}
		});
	});
}

void Upload(
		not_null<Main::Session*> session,
		uint64 docId,
		int duration,
		bool round,
		QByteArray bytes,
		Fn<void(std::optional<QString>)> done) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			done(std::nullopt);
			return;
		}
		const auto path = u"/v1/stt?doc_id=%1&duration=%2"_q
			.arg(docId)
			.arg(duration);
		Api::PostBytes(
			path,
			bytes,
			round ? u"video/mp4"_q : u"audio/ogg"_q,
			token,
			[=](QJsonObject result, int code) {
				if (code == 200) {
					done(result.value(u"text"_q).toString());
					return;
				}
				if (code == 503) {
					// The server lost its provider; stop routing here until it says so.
					GlobalStatus = Status::No;
					EngineChanges().fire({});
				}
				done(std::nullopt);
			});
	});
}

QByteArray LocalBytes(
		not_null<DocumentData*> document,
		const std::shared_ptr<Data::DocumentMedia> &media) {
	auto bytes = media->bytes();
	if (bytes.isEmpty()) {
		const auto path = document->filepath(true);
		if (!path.isEmpty()) {
			auto file = QFile(path);
			if (file.open(QIODevice::ReadOnly)) {
				bytes = file.readAll();
			}
		}
	}
	return bytes;
}

} // namespace

bool Available(not_null<Main::Session*> session) {
	if (GlobalStatus == Status::Unknown) {
		Ask(session);
	}
	return (GlobalStatus == Status::Yes);
}

bool Active(not_null<Main::Session*> session) {
	return Available(session) && !TelegramEngineValue();
}

bool ToggleEngine() {
	auto &telegram = TelegramEngineValue();
	telegram = !telegram;
	if (telegram) {
		auto file = QFile(EnginePath());
		if (file.open(QIODevice::WriteOnly)) {
			file.write("telegram");
		}
	} else {
		QFile::remove(EnginePath());
	}
	EngineChanges().fire({});
	// The button's letter is read at paint time: repaint everything once and every chat shows it.
	for (const auto widget : QApplication::topLevelWidgets()) {
		widget->update();
	}
	return telegram;
}

rpl::producer<> EngineChanged() {
	return EngineChanges().events();
}

void Transcribe(
		not_null<HistoryItem*> item,
		Fn<void(std::optional<QString>)> done) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		done(std::nullopt);
		return;
	}
	const auto session = &item->history()->session();
	const auto docId = document->id;
	const auto duration = int(document->duration() / 1000);
	const auto round = document->isVideoMessage();
	const auto view = document->createMediaView();

	auto bytes = LocalBytes(document, view);
	if (!bytes.isEmpty()) {
		Upload(session, docId, duration, round, std::move(bytes), done);
		return;
	}
	// Not here yet: fetch it the way playing it would, and go on once it is in.
	document->save(item->fullId(), QString());
	const auto lifetime = std::make_shared<rpl::lifetime>();
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=](std::optional<QByteArray> bytes) {
		if (*finished) {
			return;
		}
		*finished = true;
		lifetime->destroy();
		if (bytes && !bytes->isEmpty()) {
			Upload(session, docId, duration, round, *bytes, done);
		} else {
			done(std::nullopt);
		}
	};
	session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		if (view->loaded()) {
			finish(LocalBytes(document, view));
		}
	}, *lifetime);
	base::call_delayed(kDownloadTimeout, session, [=] {
		finish(std::nullopt);
	});
}

} // namespace Svipe::Stt
