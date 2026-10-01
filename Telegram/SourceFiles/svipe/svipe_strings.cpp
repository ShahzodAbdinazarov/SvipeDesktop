/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_strings.h"

#include "lang/lang_instance.h"

namespace Svipe {
namespace {

struct Translations {
	const char *en;
	const char *uz;
	const char *ru;
};

// Verbatim from the Android app's strings.xml files, so both apps say the same thing.
Translations Lookup(Str key) {
	switch (key) {
	case Str::MessageTypes: return {
		"Message types",
		"Xabar turlari",
		"Типы сообщений" };
	case Str::MessageTypesAll: return {
		"Message types in all chats",
		"Barcha suhbatlarda xabar turlari",
		"Типы сообщений во всех чатах" };
	case Str::MutedTypesHeader: return {
		"Muted message types",
		"Ovozsiz xabar turlari",
		"Типы сообщений без звука" };
	case Str::UnmutedTypesHeader: return {
		"Unmuted message types",
		"Ovozli xabar turlari",
		"Типы сообщений со звуком" };
	case Str::MutedTypesInfo: return {
		"These kinds never notify you, even when the chat itself is not muted.",
		"Bu turlar hech qachon bildirishnoma bermaydi — suhbatning o'zi ovozsiz qilinmagan bo'lsa ham.",
		"Эти типы никогда не уведомляют — даже если у чата звук включён." };
	case Str::UnmutedTypesInfo: return {
		"These kinds always notify you, even when the chat itself is muted.",
		"Bu turlar har doim bildirishnoma beradi — suhbatning o'zi ovozsiz qilingan bo'lsa ham.",
		"Эти типы уведомляют всегда — даже если у чата отключён звук." };
	case Str::TypeForwards: return {
		"Forwarded messages",
		"Yo'naltirilgan xabarlar",
		"Пересланные сообщения" };
	case Str::TypeLinks: return {
		"Messages with links",
		"Havolali xabarlar",
		"Сообщения со ссылками" };
	case Str::TypeMedia: return {
		"Photos and videos",
		"Rasm va videolar",
		"Фото и видео" };
	case Str::TypeVoice: return {
		"Voice and video messages",
		"Ovozli va video xabarlar",
		"Голосовые и видеосообщения" };
	case Str::TypeStickers: return {
		"Stickers and GIFs",
		"Stiker va GIFlar",
		"Стикеры и GIF" };
	case Str::TypeFiles: return {
		"Files and music",
		"Fayl va musiqa",
		"Файлы и музыка" };
	case Str::NotificationsBots: return {
		"Bots",
		"Botlar",
		"Боты" };
	case Str::NotificationsBotsMessages: return {
		"New Messages from Bots",
		"Botlardan yangi xabarlar",
		"Новые сообщения от ботов" };
	case Str::NotificationsBotsInfo: return {
		"Telegram counts bots as private chats, so silencing them means silencing your friends too. This silences only bots — new ones included.",
		"Telegram botlarni shaxsiy chat deb hisoblaydi, ya'ni ularni o'chirsangiz do'stlaringiz ham o'chadi. Bu esa faqat botlarni o'chiradi — yangilarini ham.",
		"Telegram считает ботов личными чатами, поэтому отключить их — значит отключить и друзей. Здесь отключаются только боты, включая новые." };
	case Str::NotificationsBotsExceptions: return {
		"Bots that still notify",
		"Bildirishnoma yuboradigan botlar",
		"Боты, которые уведомляют" };
	case Str::NotificationsBotsExceptionsInfo: return {
		"Bots added here keep notifying you while the rest stay silent.",
		"Bu yerga qo’shilgan botlar bildirishnoma berishda davom etadi, qolganlari esa jim turadi.",
		"Добавленные сюда боты продолжают уведомлять, остальные молчат." };
	case Str::NotificationsBotsOn: return {
		"On",
		"Yoqilgan",
		"Включены" };
	case Str::NotificationsBotsOff: return {
		"Muted",
		"O'chirilgan",
		"Отключены" };
	case Str::NotificationsBotOn: return {
		"Notifies",
		"Yuboradi",
		"Уведомляет" };
	case Str::NotificationsBotsOnlyBots: return {
		"Only bots can be added here.",
		"Bu yerga faqat botlarni qo’shish mumkin.",
		"Сюда можно добавлять только ботов." };
	case Str::MsgSyncTitle: return {
		"Deleted message sync",
		"O'chirilgan xabar sinxroni",
		"Синхронизация удалённых сообщений" };
	case Str::MsgSyncModeHeader: return {
		"Sync deleted & edited messages",
		"O'chirilgan va tahrirlangan xabarlarni sinxronlash",
		"Синхронизировать удалённые и изменённые сообщения" };
	case Str::MsgSyncWithPartner: return {
		"With the other person",
		"Suhbatdosh bilan",
		"С собеседником" };
	case Str::MsgSyncSelfOnly: return {
		"Only my own devices",
		"Faqat mening qurilmalarim",
		"Только мои устройства" };
	case Str::MsgSyncOff: return {
		"Don't sync",
		"Sinxronlanmasin",
		"Не синхронизировать" };
	case Str::MsgSyncModeInfo: return {
		"\"With the other person\" keeps a one-to-one chat's deleted and edited messages on all your devices and lets the other person see them too; you see theirs when they turn it on. \"Only my own devices\" keeps them on all your devices only. Never applies to groups, channels or Secret Chats.",
		"\"Suhbatdosh bilan\" — shaxsiy suhbatning o'chirilgan va tahrirlangan xabarlarini barcha qurilmalaringizda saqlaydi va suhbatdoshingizga ham ko'rsatadi; uning xabarlarini u ham yoqsa ko'rasiz. \"Faqat mening qurilmalarim\" — faqat barcha qurilmalaringizda saqlaydi. Guruh, kanal yoki Maxfiy chatlarga hech qachon qo'llanmaydi.",
		"«С собеседником» — хранит удалённые и изменённые сообщения личного чата на всех ваших устройствах и показывает их собеседнику; его сообщения вы увидите, когда включит он. «Только мои устройства» — хранит их только на всех ваших устройствах. Никогда не применяется к группам, каналам и секретным чатам." };
	case Str::MsgSyncOffConfirm: return {
		"If you turn sync off, the deleted and edited messages you have synced are removed from the server, your other devices stop receiving them and the other person can no longer see them. Turn it off?",
		"Sinxronni o'chirsangiz, siz sinxronlagan o'chirilgan va tahrirlangan xabarlar serverdan o'chiriladi, boshqa qurilmalaringizga kelmaydi va suhbatdoshingiz ularni ko'ra olmaydi. O'chirilsinmi?",
		"Если выключить синхронизацию, синхронизированные вами удалённые и изменённые сообщения будут удалены с сервера, перестанут приходить на другие ваши устройства и собеседник больше не сможет их видеть. Выключить?" };
	case Str::MsgSyncDelete: return {
		"Delete everything from the server",
		"Serverdan hammasini o'chirish",
		"Удалить всё с сервера" };
	case Str::MsgSyncDeleteInfo: return {
		"Removes every message you have synced. It stays on your device.",
		"Siz sinxronlagan barcha xabarni o'chiradi. Qurilmangizda qoladi.",
		"Удаляет все синхронизированные вами сообщения. На устройстве они останутся." };
	case Str::MsgSyncDeleteConfirm: return {
		"Delete every message you have synced to the server?",
		"Serverga sinxronlagan barcha xabaringiz o'chirilsinmi?",
		"Удалить все сообщения, синхронизированные вами на сервер?" };
	case Str::MsgSyncArchiveDeleted: return {
		"Synced messages deleted",
		"Sinxronlangan xabarlar o'chirildi",
		"Синхронизированные сообщения удалены" };
	case Str::MsgSyncFailed: return {
		"Couldn't reach the server. Try again.",
		"Serverga ulanib bo'lmadi. Qayta urinib ko'ring.",
		"Не удалось связаться с сервером. Попробуйте ещё раз." };
	case Str::MsgSyncPromptTitle: return {
		"Sync deleted & edited messages?",
		"O'chirilgan va tahrirlangan xabarlar sinxronlansinmi?",
		"Синхронизировать удалённые и изменённые сообщения?" };
	case Str::MsgSyncPromptMessage: return {
		"**Svipe** can keep the messages you delete or edit so you don't lose them. **Allow** — on all your devices and with the other person. **For me** — only on all your own devices. **Decline** — don't sync. You can change this any time in Settings.",
		"**Svipe** o'chirgan yoki tahrirlagan xabarlaringizni saqlab, yo'qotmasligingizga yordam beradi. **Ruxsat** — barcha qurilmalaringizda va suhbatdosh bilan. **O'zimga** — faqat o'z qurilmalaringizda. **Rad etish** — sinxronlamaslik. Istalgan payt Sozlamalarda o'zgartirasiz.",
		"**Svipe** может хранить сообщения, которые вы удаляете или редактируете, чтобы вы их не теряли. **Разрешить** — на всех ваших устройствах и с собеседником. **Мне** — только на ваших устройствах. **Отклонить** — не синхронизировать. Это можно изменить в Настройках." };
	case Str::MsgSyncSnackbar: return {
		"Sync deleted & edited messages?",
		"O'chirilgan va tahrirlangan xabarlar sinxronlansinmi?",
		"Синхронизировать удалённые и изменённые сообщения?" };
	case Str::MsgSyncAllow: return {
		"Allow",
		"Ruxsat",
		"Разрешить" };
	case Str::MsgSyncForMe: return {
		"For me",
		"O'zimga",
		"Мне" };
	case Str::MsgSyncDecline: return {
		"Decline",
		"Rad etish",
		"Отклонить" };
	case Str::MsgSyncGranted: return {
		"Sync on — you can change it in Settings",
		"Sinxron yoqildi — Sozlamalardan o'zgartirasiz",
		"Синхронизация включена — изменить можно в Настройках" };
	case Str::MsgSyncRemind: return {
		"Don't ask for a month",
		"Bir oy so'ralmasin",
		"Не спрашивать месяц" };
	case Str::MsgSyncRemindInfo: return {
		"Turn this on to stop all sync prompts for a month.",
		"Yoqsangiz, bir oygacha sinxronlash umuman so'ralmaydi.",
		"Включите, чтобы месяц не показывать запросы синхронизации." };
	case Str::MsgSyncNotSet: return {
		"Not set",
		"Tanlanmagan",
		"Не выбрано" };
	case Str::ProfileImages: return {
		"Profile Images",
		"Profil rasmlari",
		"Фото профиля" };
	case Str::DeletedLabel: return {
		"Deleted",
		"O'chirilgan",
		"Удалено" };
	case Str::ShowInChat: return {
		"Show in chat",
		"Chatda ko'rsatish",
		"Показывать в чате" };
	case Str::RecentActionsEmpty: return {
		"No deleted or edited messages here yet",
		"Bu yerda hali o'chgan yoki tahrirlangan xabar yo'q",
		"Здесь пока нет удалённых или изменённых сообщений" };
	case Str::YouDeletedMessage: return {
		"You deleted this message:",
		"Siz bu xabarni o'chirdingiz:",
		"Вы удалили это сообщение:" };
	case Str::YouEditedMessage: return {
		"You edited this message:",
		"Siz bu xabarni tahrirladingiz:",
		"Вы изменили это сообщение:" };
	case Str::NotificationsBotsTitle: return {
		"Notifications for bots",
		"Botlar uchun bildirishnomalar",
		"Уведомления от ботов" };
	}
	Unexpected("Key in Svipe::Tr.");
}

enum class Language {
	English,
	Uzbek,
	Russian,
};

Language Current() {
	const auto &lang = Lang::GetInstance();
	const auto id = (lang.id() + u"|"_q + lang.baseId()).toLower();
	if (id.startsWith(u"uz"_q) || id.contains(u"|uz"_q)) {
		return Language::Uzbek;
	} else if (id.startsWith(u"ru"_q) || id.contains(u"|ru"_q)) {
		return Language::Russian;
	}
	return Language::English;
}

} // namespace

QString Tr(Str key) {
	const auto value = Lookup(key);
	switch (Current()) {
	case Language::Uzbek: return QString::fromUtf8(value.uz);
	case Language::Russian: return QString::fromUtf8(value.ru);
	case Language::English: break;
	}
	return QString::fromUtf8(value.en);
}

QString BotsExceptionsCount(int count) {
	const auto n = QString::number(count);
	switch (Current()) {
	case Language::Uzbek:
		return n + u" ta istisno"_q;
	case Language::Russian: {
		const auto mod10 = count % 10;
		const auto mod100 = count % 100;
		if (mod10 == 1 && mod100 != 11) {
			return n + QString::fromUtf8(" исключение");
		} else if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
			return n + QString::fromUtf8(" исключения");
		}
		return n + QString::fromUtf8(" исключений");
	}
	case Language::English: break;
	}
	return n + (count == 1 ? u" exception"_q : u" exceptions"_q);
}

QString ProfileImagesCount(int count) {
	const auto n = QString::number(count);
	switch (Current()) {
	case Language::Uzbek:
		return n + u" ta rasm"_q;
	case Language::Russian:
		return n + QString::fromUtf8(" фото"); // indeclinable
	case Language::English: break;
	}
	return n + (count == 1 ? u" image"_q : u" images"_q);
}

rpl::producer<QString> TrValue(Str key) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Lang::GetInstance().updated()
	) | rpl::map([=] {
		return Tr(key);
	});
}

} // namespace Svipe
