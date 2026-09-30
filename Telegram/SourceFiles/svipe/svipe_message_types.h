/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Notification rules by KIND of message, ported one to one from the Android app
(NotificationsController.MESSAGE_KINDS / kindRule / isMutedMessageType / isNotifiedMessageType and
SvipeMessageTypeMute.java).

Two lists per kind:
  - muted:    in a chat that is NOT muted, messages of this kind do not notify;
  - notified: in a chat that IS muted, messages of this kind notify anyway.
A rule is written for one chat (its Android dialog id, so the lists sync between the two apps) or for
a class of chats: "all", "private", "groups", "channels", "bots". A chat's own rule is read first,
then its class, then "all". The rules are client-side only — Telegram has no field for them — and
travel between a user's Svipe installs through Svipe::SettingsSync.
*/
#pragma once

#include "svipe/svipe_strings.h"
#include "ui/style/style_core_icon.h"

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main


namespace Svipe::MessageTypes {

[[nodiscard]] const std::vector<QString> &Kinds();
[[nodiscard]] Str LabelOf(const QString &kind);
[[nodiscard]] const style::icon *IconOf(const QString &kind);

inline const auto kScopeAll = u"all"_q;
inline const auto kScopePrivate = u"private"_q;
inline const auto kScopeGroups = u"groups"_q;
inline const auto kScopeChannels = u"channels"_q;
inline const auto kScopeBots = u"bots"_q;

// The rule target for one chat, as the Android app spells it (a dialog id).
[[nodiscard]] QString TargetOf(not_null<PeerData*> peer);
[[nodiscard]] QString ScopeOf(not_null<PeerData*> peer);

// The storage prefix of one list: "svipe_mute_<kind>_" or "svipe_notify_<kind>_".
[[nodiscard]] QString PrefixOf(bool muted, const QString &kind);

[[nodiscard]] bool Has(
	not_null<Main::Session*> session,
	bool muted,
	const QString &kind,
	const QString &target);
void Set(
	not_null<Main::Session*> session,
	bool muted,
	const QString &kind,
	const QString &target,
	bool on);

[[nodiscard]] bool IsKind(not_null<const HistoryItem*> item, const QString &kind);

// In a chat that is not muted: should this message stay silent?
[[nodiscard]] bool IsMutedType(not_null<const HistoryItem*> item);
// In a chat that is muted: should this message notify anyway?
[[nodiscard]] bool IsNotifiedType(not_null<const HistoryItem*> item);

// Fires whenever a rule changes on this device or arrives from another one.
[[nodiscard]] rpl::producer<> Changes();

// For the sync: every target on one list, and adopting a list from another device.
[[nodiscard]] QStringList TargetsOf(
	not_null<Main::Session*> session,
	const QString &prefix);
void AdoptList(
	not_null<Main::Session*> session,
	const QString &prefix,
	const QStringList &targets);
[[nodiscard]] qint64 UpdatedAt(not_null<Main::Session*> session);
void SetUpdatedAt(not_null<Main::Session*> session, qint64 value);

} // namespace Svipe::MessageTypes
