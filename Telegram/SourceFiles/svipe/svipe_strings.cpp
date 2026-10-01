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
