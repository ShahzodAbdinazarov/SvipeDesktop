/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Svipe's own UI strings, in the three languages the Android app ships (values, values-uz, values-ru).
Telegram's cloud language packs know nothing about them, so the translation is picked here from the
app's current language, English otherwise.
*/
#pragma once

namespace Svipe {

enum class Str {
	MessageTypes,
	MessageTypesAll,
	MutedTypesHeader,
	UnmutedTypesHeader,
	MutedTypesInfo,
	UnmutedTypesInfo,
	TypeForwards,
	TypeLinks,
	TypeMedia,
	TypeVoice,
	TypeStickers,
	TypeFiles,
	NotificationsBots,
	NotificationsBotsMessages,
	NotificationsBotsInfo,
	NotificationsBotsExceptions,
	NotificationsBotsExceptionsInfo,
	NotificationsBotsOn,
	NotificationsBotsOff,
	NotificationsBotOn,
	NotificationsBotsOnlyBots,
	NotificationsBotsTitle,
	MsgSyncTitle,
	MsgSyncModeHeader,
	MsgSyncWithPartner,
	MsgSyncSelfOnly,
	MsgSyncOff,
	MsgSyncModeInfo,
	MsgSyncOffConfirm,
	MsgSyncDelete,
	MsgSyncDeleteInfo,
	MsgSyncDeleteConfirm,
	MsgSyncArchiveDeleted,
	MsgSyncFailed,
	MsgSyncPromptTitle,
	MsgSyncPromptMessage,
	MsgSyncSnackbar,
	MsgSyncAllow,
	MsgSyncForMe,
	MsgSyncDecline,
	MsgSyncGranted,
	MsgSyncRemind,
	MsgSyncRemindInfo,
	MsgSyncNotSet,
	DeletedLabel,
	ProfileImages,
	ShowInChat,
	RecentActionsEmpty,
	YouDeletedMessage,
	YouEditedMessage,
};

// "N exceptions", with each language's plural forms.
[[nodiscard]] QString BotsExceptionsCount(int count);
[[nodiscard]] QString ProfileImagesCount(int count);

[[nodiscard]] QString Tr(Str key);
[[nodiscard]] rpl::producer<QString> TrValue(Str key);

} // namespace Svipe
