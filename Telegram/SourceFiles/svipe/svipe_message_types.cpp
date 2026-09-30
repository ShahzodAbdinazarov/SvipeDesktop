/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_message_types.h"

#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "svipe/svipe_settings_sync.h"
#include "svipe/svipe_storage.h"
#include "styles/style_menu_icons.h"

namespace Svipe::MessageTypes {
namespace {

const auto kUpdatedAt = u"svipe_type_mute_updated"_q;

rpl::event_stream<> &ChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] bool Rule(
		not_null<Main::Session*> session,
		bool muted,
		const QString &kind,
		not_null<PeerData*> peer) {
	return Has(session, muted, kind, TargetOf(peer))
		|| Has(session, muted, kind, ScopeOf(peer))
		|| Has(session, muted, kind, kScopeAll);
}

[[nodiscard]] bool Matches(not_null<const HistoryItem*> item, bool muted) {
	if (item->out()) {
		return false;
	}
	const auto session = &item->history()->session();
	const auto peer = item->history()->peer;
	for (const auto &kind : Kinds()) {
		if (Rule(session, muted, kind, peer) && IsKind(item, kind)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool CarriesLink(not_null<const HistoryItem*> item) {
	if (const auto media = item->media()) {
		if (media->webpage()) {
			return true;
		}
	}
	for (const auto &entity : item->originalText().entities) {
		const auto type = entity.type();
		if (type == EntityType::Url || type == EntityType::CustomUrl) {
			return true;
		}
	}
	return false;
}

} // namespace

const std::vector<QString> &Kinds() {
	static const auto result = std::vector<QString>{
		u"forwards"_q,
		u"links"_q,
		u"media"_q,
		u"voice"_q,
		u"stickers"_q,
		u"files"_q,
	};
	return result;
}

Str LabelOf(const QString &kind) {
	if (kind == u"links"_q) return Str::TypeLinks;
	if (kind == u"media"_q) return Str::TypeMedia;
	if (kind == u"voice"_q) return Str::TypeVoice;
	if (kind == u"stickers"_q) return Str::TypeStickers;
	if (kind == u"files"_q) return Str::TypeFiles;
	return Str::TypeForwards;
}

const style::icon *IconOf(const QString &kind) {
	if (kind == u"links"_q) return &st::menuIconLink;
	if (kind == u"media"_q) return &st::menuIconPhoto;
	if (kind == u"voice"_q) return &st::menuIconUnmute;
	if (kind == u"stickers"_q) return &st::menuIconStickers;
	if (kind == u"files"_q) return &st::menuIconFile;
	return &st::menuIconForward;
}

QString TargetOf(not_null<PeerData*> peer) {
	// The Android client's dialog id: a user's id, or minus the id of a chat or channel.
	const auto id = peer->id;
	if (peerIsUser(id)) {
		return QString::number(peerToUser(id).bare);
	} else if (peerIsChat(id)) {
		return QString::number(-qint64(peerToChat(id).bare));
	}
	return QString::number(-qint64(peerToChannel(id).bare));
}

QString ScopeOf(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->isBot() ? kScopeBots : kScopePrivate;
	}
	return peer->isBroadcast() ? kScopeChannels : kScopeGroups;
}

QString PrefixOf(bool muted, const QString &kind) {
	return (muted ? u"svipe_mute_"_q : u"svipe_notify_"_q) + kind + '_';
}

bool Has(
		not_null<Main::Session*> session,
		bool muted,
		const QString &kind,
		const QString &target) {
	return Storage::Get(session, PrefixOf(muted, kind) + target).toBool();
}

void Set(
		not_null<Main::Session*> session,
		bool muted,
		const QString &kind,
		const QString &target,
		bool on) {
	const auto key = PrefixOf(muted, kind) + target;
	if (on) {
		Storage::Set(session, key, true);
	} else {
		Storage::Remove(session, key);
	}
	SetUpdatedAt(session, NowMs());
	ChangesStream().fire({});
	SettingsSync::Push(session);
}

bool IsKind(not_null<const HistoryItem*> item, const QString &kind) {
	if (kind == u"forwards"_q) {
		return item->Has<HistoryMessageForwarded>();
	} else if (kind == u"links"_q) {
		return CarriesLink(item);
	}
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (kind == u"media"_q) {
		return photo
			|| (document
				&& document->isVideoFile()
				&& !document->isVideoMessage());
	} else if (!document) {
		return false;
	} else if (kind == u"voice"_q) {
		return document->isVoiceMessage() || document->isVideoMessage();
	} else if (kind == u"stickers"_q) {
		return document->sticker() || document->isAnimation();
	} else if (kind == u"files"_q) {
		return document->isAudioFile()
			|| (!document->sticker()
				&& !document->isAnimation()
				&& !document->isVoiceMessage()
				&& !document->isVideoMessage()
				&& !document->isVideoFile());
	}
	return false;
}

bool IsMutedType(not_null<const HistoryItem*> item) {
	return Matches(item, true);
}

bool IsNotifiedType(not_null<const HistoryItem*> item) {
	return Matches(item, false);
}

rpl::producer<> Changes() {
	return ChangesStream().events();
}

QStringList TargetsOf(
		not_null<Main::Session*> session,
		const QString &prefix) {
	auto result = QStringList();
	for (const auto &key : Storage::KeysWithPrefix(session, prefix)) {
		const auto target = key.mid(prefix.size());
		// Topic keys ("dialog_topic") stay on the device, as on Android.
		if (!target.contains('_') && Storage::Get(session, key).toBool()) {
			result.push_back(target);
		}
	}
	return result;
}

void AdoptList(
		not_null<Main::Session*> session,
		const QString &prefix,
		const QStringList &targets) {
	// Whatever is not on the list is no longer set, so a switch turned OFF elsewhere travels as
	// well as one turned on.
	for (const auto &target : TargetsOf(session, prefix)) {
		if (!targets.contains(target)) {
			Storage::Remove(session, prefix + target);
		}
	}
	for (const auto &target : targets) {
		if (!target.isEmpty()) {
			Storage::Set(session, prefix + target, true);
		}
	}
	ChangesStream().fire({});
}

qint64 UpdatedAt(not_null<Main::Session*> session) {
	return qint64(Storage::Get(session, kUpdatedAt).toDouble());
}

void SetUpdatedAt(not_null<Main::Session*> session, qint64 value) {
	Storage::Set(session, kUpdatedAt, double(value));
}

} // namespace Svipe::MessageTypes
