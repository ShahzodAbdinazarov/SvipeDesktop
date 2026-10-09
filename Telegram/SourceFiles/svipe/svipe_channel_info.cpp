/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_channel_info.h"

#include "settings.h"
#include "svipe/svipe_api.h"
#include "ui/painter.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace Svipe::ChannelInfo {
namespace {

// Android: SvipeChannelAvatar.FAIL_TTL_MS — a failure says nothing about the channel.
constexpr auto kFailTtl = crl::time(60 * 1000);
constexpr auto kDiskTtlDays = 7;
constexpr auto kPhotoSize = 160;

struct State {
	base::flat_map<QString, Info> known;
	base::flat_map<QString, crl::time> misses; // handle -> retry-after
	base::flat_set<QString> pending;
	rpl::event_stream<QString> updated;
};

State &Instance() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString Normalize(const QString &handle) {
	auto result = handle.trimmed().toLower();
	if (result.startsWith('@')) {
		result = result.mid(1);
	}
	static const auto valid = QRegularExpression(u"^[a-z0-9_]{3,64}$"_q);
	return valid.match(result).hasMatch() ? result : QString();
}

[[nodiscard]] QString CacheDir() {
	return cWorkingDir() + u"tdata/svipe_channels/"_q;
}

// Android: SvipeChannelAvatar.isAllowed — only hosts Telegram serves pictures from.
[[nodiscard]] bool Allowed(const QString &url) {
	const auto parsed = QUrl(url);
	if (parsed.scheme() != u"https"_q || !parsed.userInfo().isEmpty()) {
		return false;
	}
	const auto host = parsed.host().toLower();
	return (host == u"t.me"_q)
		|| host.endsWith(u".telesco.pe"_q)
		|| host.endsWith(u".cdn-telegram.org"_q)
		|| host.endsWith(u".telegram-cdn.org"_q)
		|| host.endsWith(u".t.me"_q);
}

[[nodiscard]] QString Unescape(QString text) {
	return text
		.replace(u"&amp;"_q, u"&"_q)
		.replace(u"&quot;"_q, u"\""_q)
		.replace(u"&#39;"_q, u"'"_q)
		.replace(u"&lt;"_q, u"<"_q)
		.replace(u"&gt;"_q, u">"_q);
}

[[nodiscard]] QString ParsePhotoUrl(const QString &html) {
	static const auto patterns = std::array{
		QRegularExpression(
			u"<img class=\"tgme_page_photo_image\"[^>]*src=\"([^\"]+)\""_q,
			QRegularExpression::CaseInsensitiveOption),
		QRegularExpression(
			u"<i class=\"tgme_page_photo_image[^\"]*\"[^>]*>\\s*<img[^>]+src=\"([^\"]+)\""_q,
			QRegularExpression::CaseInsensitiveOption),
	};
	for (const auto &pattern : patterns) {
		const auto match = pattern.match(html);
		if (match.hasMatch()) {
			const auto url = Unescape(match.captured(1));
			if (Allowed(url)) {
				return url;
			}
		}
	}
	return QString();
}

[[nodiscard]] QString ParseTitle(const QString &html) {
	static const auto pattern = QRegularExpression(
		u"<meta property=\"og:title\" content=\"([^\"]*)\""_q);
	const auto match = pattern.match(html);
	return match.hasMatch() ? Unescape(match.captured(1)).trimmed() : QString();
}

[[nodiscard]] QImage Prepare(QImage image) {
	if (image.isNull()) {
		return image;
	}
	const auto side = std::min(image.width(), image.height());
	image = image.copy(
		(image.width() - side) / 2,
		(image.height() - side) / 2,
		side,
		side);
	return image.scaled(
		kPhotoSize,
		kPhotoSize,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

void Store(const QString &handle, Info info) {
	auto &state = Instance();
	state.pending.remove(handle);
	state.known[handle] = std::move(info);
	state.updated.fire_copy(handle);
}

void Miss(const QString &handle, crl::time ttl) {
	auto &state = Instance();
	state.pending.remove(handle);
	state.misses[handle] = crl::now() + ttl;
}

[[nodiscard]] bool LoadFromDisk(const QString &handle) {
	const auto base = CacheDir() + handle;
	const auto photo = QFileInfo(base + u".jpg"_q);
	const auto title = QFileInfo(base + u".txt"_q);
	if (!title.exists()
		|| title.lastModified().daysTo(QDateTime::currentDateTime()) >= kDiskTtlDays) {
		return false;
	}
	auto info = Info();
	auto file = QFile(title.filePath());
	if (file.open(QIODevice::ReadOnly)) {
		info.title = QString::fromUtf8(file.readAll());
	}
	if (photo.exists()) {
		info.photo = Prepare(QImage(photo.filePath()));
	}
	Instance().known[handle] = std::move(info);
	return true;
}

void SaveToDisk(const QString &handle, const QByteArray &photo, const QString &title) {
	QDir().mkpath(CacheDir());
	const auto base = CacheDir() + handle;
	if (!photo.isEmpty()) {
		auto file = QFile(base + u".jpg"_q);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(photo);
		}
	} else {
		QFile::remove(base + u".jpg"_q);
	}
	auto file = QFile(base + u".txt"_q);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(title.toUtf8());
	}
}

void Request(const QString &handle) {
	auto &state = Instance();
	if (state.pending.contains(handle)) {
		return;
	}
	state.pending.emplace(handle);
	Api::GetBytes(u"https://t.me/"_q + handle, [=](QByteArray page, int code) {
		if (code != 200 || page.isEmpty()) {
			Miss(handle, kFailTtl); // the network, not the channel
			return;
		}
		const auto html = QString::fromUtf8(page);
		const auto title = ParseTitle(html);
		const auto url = ParsePhotoUrl(html);
		if (url.isEmpty()) {
			// A stable fact, kept on disk like a picture: this channel has none to show.
			SaveToDisk(handle, QByteArray(), title);
			Store(handle, Info{ .title = title });
			return;
		}
		Api::GetBytes(url, [=](QByteArray bytes, int code) {
			auto image = (code == 200) ? QImage::fromData(bytes) : QImage();
			if (image.isNull()) {
				Miss(handle, kFailTtl);
				return;
			}
			SaveToDisk(handle, bytes, title);
			Store(handle, Info{ .photo = Prepare(std::move(image)), .title = title });
		});
	});
}

} // namespace

const Info *Lookup(const QString &handle) {
	const auto normalized = Normalize(handle);
	if (normalized.isEmpty()) {
		return nullptr;
	}
	auto &state = Instance();
	const auto i = state.known.find(normalized);
	if (i != end(state.known)) {
		return &i->second;
	}
	if (LoadFromDisk(normalized)) {
		return &state.known[normalized];
	}
	const auto miss = state.misses.find(normalized);
	if (miss != end(state.misses) && miss->second > crl::now()) {
		return nullptr;
	}
	Request(normalized);
	return nullptr;
}

rpl::producer<QString> Updated() {
	return Instance().updated.events();
}

void PaintUserpic(QPainter &p, const QString &handle, QRect rect, const QBrush &placeholder) {
	auto hq = PainterHighQualityEnabler(p);
	const auto info = Lookup(handle);
	if (info && !info->photo.isNull()) {
		auto path = QPainterPath();
		path.addEllipse(rect);
		p.save();
		p.setClipPath(path);
		p.drawImage(rect, info->photo);
		p.restore();
		return;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(placeholder);
	p.drawEllipse(rect);
}

} // namespace Svipe::ChannelInfo
