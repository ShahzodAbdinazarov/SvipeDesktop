/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A public channel's picture and name with no contacts.resolveUsername behind them (Android:
SvipeChannelAvatar). The plain page https://t.me/<handle> carries both — the picture as
<img class="tgme_page_photo_image" src="…"> and the name as og:title — for one HTTPS GET with no
session, no access_hash and nothing taken from the flood budget. It must be the plain page: t.me/s/
renders the picture slot as a letter placeholder with no src.

Pictures and names are kept in memory and on disk (tdata/svipe_channels), so a channel is read once a
week, not once a launch — including the fact that it has no picture.
*/
#pragma once

namespace Svipe::ChannelInfo {

struct Info {
	QImage photo; // empty when the channel has none we can read
	QString title;
};

// What is known now, or nullptr; asks for it in the background when not known. `updated()` fires
// with the handle when an answer arrives.
[[nodiscard]] const Info *Lookup(const QString &handle);
[[nodiscard]] rpl::producer<QString> Updated();

// Draw the picture as a circle, or a neutral circle while there is none.
void PaintUserpic(QPainter &p, const QString &handle, QRect rect, const QBrush &placeholder);

} // namespace Svipe::ChannelInfo
