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
	}
	Unexpected("Key in Svipe::Tr.");
}

} // namespace

QString Tr(Str key) {
	const auto &lang = Lang::GetInstance();
	const auto id = (lang.id() + u"|"_q + lang.baseId()).toLower();
	const auto value = Lookup(key);
	if (id.startsWith(u"uz"_q) || id.contains(u"|uz"_q)) {
		return QString::fromUtf8(value.uz);
	} else if (id.startsWith(u"ru"_q) || id.contains(u"|ru"_q)) {
		return QString::fromUtf8(value.ru);
	}
	return QString::fromUtf8(value.en);
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
