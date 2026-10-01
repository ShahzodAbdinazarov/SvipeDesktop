/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_avatar_archive.h"

#include "base/unixtime.h"
#include "data/data_file_origin.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/image/image.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Svipe::AvatarArchive {
namespace {

// Android: SvipeAvatarStore.MAX_PER_USER — generous, a person rarely has this many avatars.
constexpr auto kMaxPerUser = 60;

struct Pending {
	UserId user = 0;
	PhotoId photo = 0;
	std::shared_ptr<Data::PhotoMedia> media;
};

struct State {
	base::flat_map<UserId, LiveSet> live;
	std::vector<Pending> pending;
	base::flat_set<not_null<Main::Session*>> watched;
	rpl::event_stream<UserId> updates;
};

State &GlobalState() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString Dir() {
	return cWorkingDir() + u"tdata/svipe/avatars/"_q;
}

[[nodiscard]] QString LedgerPath(UserId user) {
	return Dir() + QString::number(user.bare) + u".json"_q;
}

[[nodiscard]] std::vector<Photo> ReadLedger(UserId user) {
	auto file = QFile(LedgerPath(user));
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto result = std::vector<Photo>();
	for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
		const auto object = value.toObject();
		const auto id = PhotoId(object.value(u"id"_q).toString().toULongLong());
		if (id) {
			result.push_back({
				.id = id,
				.date = TimeId(object.value(u"date"_q).toDouble()),
				.capturedAt = TimeId(object.value(u"seen"_q).toDouble()),
			});
		}
	}
	return result;
}

void WriteLedger(UserId user, const std::vector<Photo> &photos) {
	QDir().mkpath(Dir());
	auto array = QJsonArray();
	for (const auto &photo : photos) {
		array.push_back(QJsonObject{
			// As a string: a photo id is a full 64-bit value and a JSON double would round it.
			{ u"id"_q, QString::number(photo.id) },
			{ u"date"_q, double(photo.date) },
			{ u"seen"_q, double(photo.capturedAt) },
		});
	}
	auto file = QFile(LedgerPath(user));
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
	}
}

// SvipeAvatarStore.record: append if new, drop the oldest captured over the cap.
bool Record(UserId user, PhotoId id, TimeId date) {
	auto photos = ReadLedger(user);
	if (ranges::contains(photos, id, &Photo::id)) {
		return false;
	}
	photos.push_back({ .id = id, .date = date, .capturedAt = base::unixtime::now() });
	if (photos.size() > kMaxPerUser) {
		ranges::sort(photos, ranges::less(), &Photo::capturedAt);
		photos.erase(begin(photos), begin(photos) + (photos.size() - kMaxPerUser));
	}
	WriteLedger(user, photos);
	return true;
}

// The largest size, as the server sent it when we have the bytes, re-encoded otherwise.
bool Save(UserId user, PhotoId photo, const std::shared_ptr<Data::PhotoMedia> &media) {
	const auto path = FilePath(user, photo);
	const auto bytes = media->imageBytes(Data::PhotoSize::Large);
	if (!bytes.isEmpty()) {
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			file.write(bytes);
			return true;
		}
		return false;
	}
	if (const auto image = media->image(Data::PhotoSize::Large)) {
		return image->original().save(path, "JPG", 90);
	}
	return false;
}

void CheckPending() {
	auto &state = GlobalState();
	auto saved = base::flat_set<UserId>();
	for (auto i = begin(state.pending); i != end(state.pending);) {
		if (i->media->image(Data::PhotoSize::Large)) {
			if (Save(i->user, i->photo, i->media)) {
				saved.emplace(i->user);
			}
			i = state.pending.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &user : saved) {
		state.updates.fire_copy(user);
	}
}

void Watch(not_null<Main::Session*> session) {
	auto &state = GlobalState();
	if (!state.watched.emplace(session).second) {
		return;
	}
	session->downloaderTaskFinished(
	) | rpl::on_next([] {
		CheckPending();
	}, session->lifetime());
	session->lifetime().add([=] {
		auto &state = GlobalState();
		state.watched.remove(session);
		state.pending.erase(ranges::remove_if(state.pending, [&](const Pending &p) {
			return &p.media->owner()->session() == session;
		}), end(state.pending));
	});
}

void Persist(not_null<UserData*> user, not_null<PhotoData*> photo) {
	const auto userId = peerToUser(user->id);
	if (HasFile(userId, photo->id)) {
		return;
	}
	QDir().mkpath(Dir());
	auto media = photo->createMediaView();
	media->wanted(
		Data::PhotoSize::Large,
		Data::FileOriginUserPhoto(userId, photo->id));
	if (media->image(Data::PhotoSize::Large)) {
		Save(userId, photo->id, media);
		return;
	}
	Watch(&user->session());
	auto &pending = GlobalState().pending;
	const auto already = ranges::any_of(pending, [&](const Pending &p) {
		return (p.user == userId) && (p.photo == photo->id);
	});
	if (!already) {
		pending.push_back({ userId, photo->id, std::move(media) });
	}
}

} // namespace

void Remember(
		not_null<UserData*> user,
		const std::vector<not_null<PhotoData*>> &photos,
		int fullCount,
		bool firstPage) {
	const auto userId = peerToUser(user->id);
	auto &state = GlobalState();
	auto &live = state.live[userId];
	if (firstPage) {
		live.ids.clear();
	}
	for (const auto &photo : photos) {
		if (photo->isNull() || !photo->id) {
			continue;
		}
		if (!ranges::contains(live.ids, photo->id)) {
			live.ids.push_back(photo->id);
		}
		Record(userId, photo->id, photo->date());
		Persist(user, photo);
	}
	// The list arrives in pages; only a list holding every photo the server counts proves that a
	// captured photo missing from it was deleted (SvipeAvatarSync.liveSetComplete).
	live.complete = (fullCount > 0) && (int(live.ids.size()) >= fullCount);
	state.updates.fire_copy(userId);
}

void RecordSynced(UserId user, PhotoId photo, TimeId date) {
	Record(user, photo, date);
	GlobalState().updates.fire_copy(user);
}

std::vector<Photo> Captured(UserId user) {
	auto result = ReadLedger(user);
	// Newest set first, stable so the grid never reshuffles (SvipeAvatarStore.sortBySetOrder).
	ranges::sort(result, [](const Photo &a, const Photo &b) {
		return (a.date != b.date)
			? (a.date > b.date)
			: (a.capturedAt != b.capturedAt)
			? (a.capturedAt > b.capturedAt)
			: (a.id > b.id);
	});
	return result;
}

QString FilePath(UserId user, PhotoId photo) {
	return Dir() + u"%1_%2.jpg"_q.arg(user.bare).arg(photo);
}

bool HasFile(UserId user, PhotoId photo) {
	const auto info = QFileInfo(FilePath(user, photo));
	return info.exists() && info.size() > 0;
}

LiveSet Live(not_null<UserData*> user) {
	const auto &live = GlobalState().live;
	const auto i = live.find(peerToUser(user->id));
	return (i != end(live)) ? i->second : LiveSet();
}

std::vector<Photo> Deleted(not_null<UserData*> user) {
	const auto live = Live(user);
	if (!live.complete) {
		return {};
	}
	const auto userId = peerToUser(user->id);
	auto result = std::vector<Photo>();
	for (const auto &photo : Captured(userId)) {
		if (!ranges::contains(live.ids, photo.id) && HasFile(userId, photo.id)) {
			result.push_back(photo);
		}
	}
	return result;
}

rpl::producer<UserId> Updates() {
	return GlobalState().updates.events();
}

} // namespace Svipe::AvatarArchive
