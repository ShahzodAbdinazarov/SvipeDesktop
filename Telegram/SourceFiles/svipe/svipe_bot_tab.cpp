/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bot_tab.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "info/info_controller.h"
#include "info/media/info_media_buttons.h"
#include "info/profile/info_profile_icon.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_strings.h"
#include "svipe/svipe_web_tab.h"
#include "ui/basic_click_handlers.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/layers/show.h"
#include "window/window_session_controller.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtGui/QLinearGradient>

namespace Svipe::BotTab {
namespace {

constexpr auto kColumns = 3;
constexpr auto kMaxProfileTabs = 3;
// Tabs one profile can show: a bot's own, three of each kind, and the viewer's private ones.
constexpr auto kMaxShownTabs = 6;
constexpr auto kOpenedFromTimeout = crl::time(30'000);

struct Card {
	QString id;
	QString title;
	QString subtitle;
	QString badge;
	QString link;
	QString image;
};

struct State {
	bool active = false;
	QString botUsername;
	// "channel", "supergroup", "group", "saved"; on a person's profile the place it is in
	// ("profile", "private", "saved", "bot"): the chat_type its mini apps are signed in with.
	QString chatType;
	QString place;
	// What the bot shows this viewer when it has no cards for them (live cards).
	QString emptyText;
	QString emptyImage;
	// Mode "web": the bot's page in the tab instead of cards.
	QString webUrl;
	QString title;
	bool list = false;
	bool isAttacher = false;
	bool canRename = false;
	int total = 0;
	std::vector<Card> cards;
	QString myRequestBot;
	QString myRequestState;
	// On a person's profile: whose profile, and which bot's tab.
	uint64 ownerId = 0;
	uint64 botId = 0;
};

struct MyTab {
	uint64 botId = 0;
	QString username;
	QString title;
	bool everyone = true;
	QString kind; // "profile" or "private"
	QString place; // on a profile I look at: where it is
};

// A tab a profile screen shows me: whose bot, and where.
struct Shown {
	uint64 botId = 0;
	QString place;
};

struct Opened {
	PeerId peer;
	QString chatType;
	crl::time when = 0;
};

// States by key: "c<chat>" for a channel's or group's tab, "s" for Saved Messages' settings,
// "u<owner><view>_<bot>_<place>" for a tab on a profile screen.
struct Global {
	base::flat_map<QString, std::shared_ptr<State>> states;
	base::flat_set<QString> loadingMore;
	base::flat_map<QString, QImage> images;
	base::flat_set<QString> imagesLoading;
	rpl::event_stream<QString> updates;
	rpl::event_stream<> imageLoaded;

	base::flat_map<QString, std::vector<Shown>> profiles; // profile prefix -> the tabs I see there
	std::vector<MyTab> mine;
	bool mineLoaded = false;
	base::flat_set<QString> declined; // "<bot>:<kind>" asked from a mini app and said no, this run
	base::flat_map<uint64, Opened> openedFrom; // bot -> where its card was opened
};

Global &G() {
	static auto result = Global();
	return result;
}

[[nodiscard]] QString OptString(const QJsonObject &o, const QString &key) {
	const auto value = o.value(key);
	return value.isString() ? value.toString() : QString();
}

// The chat's tab on the server: a channel or supergroup by its id, a basic group by its negative id.
[[nodiscard]] qint64 ChatId(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return qint64(peerToChannel(channel->id).bare);
	} else if (const auto chat = peer->asChat()) {
		return -qint64(peerToChat(chat->id).bare);
	}
	return 0;
}

[[nodiscard]] QString ChatType(not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return u"saved"_q;
	} else if (const auto channel = peer->asChannel()) {
		return channel->isBroadcast() ? u"channel"_q : u"supergroup"_q;
	}
	return u"group"_q;
}

[[nodiscard]] QString ChannelKey(not_null<PeerData*> peer) {
	return peer->isSelf() ? u"s"_q : u"c%1"_q.arg(ChatId(peer));
}

[[nodiscard]] std::shared_ptr<State> Parse(
		const QJsonObject &res,
		bool forceActive = false) {
	auto state = std::make_shared<State>();
	state->active = forceActive
		|| (res.value(u"state"_q).toString() == u"active"_q);
	state->botUsername = OptString(res.value(u"bot"_q).toObject(), u"username"_q);
	state->botId = uint64(res.value(u"bot"_q).toObject().value(u"bot_id"_q).toVariant().toLongLong());
	state->chatType = OptString(res, u"chat_type"_q);
	state->place = OptString(res, u"place"_q);
	const auto empty = res.value(u"empty"_q).toObject();
	state->emptyText = OptString(empty, u"text"_q);
	state->emptyImage = OptString(empty, u"image"_q);
	if (res.value(u"mode"_q).toString() == u"web"_q) {
		state->webUrl = OptString(res, u"web_url"_q);
	}
	state->title = OptString(res, u"title"_q);
	if (state->title.isEmpty()) {
		state->title = state->botUsername;
	}
	state->list = (res.value(u"layout"_q).toString() == u"list"_q);
	state->isAttacher = res.value(u"is_attacher"_q).toBool();
	state->canRename = res.value(u"can_rename"_q).toBool();
	state->total = res.value(u"total"_q).toInt();
	const auto mine = res.value(u"my_request"_q).toObject();
	state->myRequestBot = OptString(mine, u"bot_username"_q);
	state->myRequestState = OptString(mine, u"state"_q);
	for (const auto &value : res.value(u"items"_q).toArray()) {
		const auto o = value.toObject();
		state->cards.push_back({
			.id = OptString(o, u"id"_q),
			.title = OptString(o, u"title"_q),
			.subtitle = OptString(o, u"subtitle"_q),
			.badge = OptString(o, u"badge"_q),
			.link = OptString(o, u"link"_q),
			.image = OptString(o, u"image"_q),
		});
	}
	return state;
}

[[nodiscard]] std::shared_ptr<State> Cached(const QString &key) {
	const auto i = G().states.find(key);
	return (i != end(G().states)) ? i->second : nullptr;
}

[[nodiscard]] bool IsWeb(const std::shared_ptr<State> &state) {
	return state && !state->webUrl.isEmpty() && state->botId;
}

[[nodiscard]] bool ShowsEmpty(const std::shared_ptr<State> &state) {
	return state
		&& !IsWeb(state)
		&& state->cards.empty()
		&& (!state->emptyText.isEmpty() || !state->emptyImage.isEmpty());
}

[[nodiscard]] std::shared_ptr<State> Cached(not_null<PeerData*> peer) {
	return Cached(ChannelKey(peer));
}

[[nodiscard]] QString TabPath(not_null<PeerData*> peer) {
	return peer->isSelf()
		? u"/v1/chat-tabs/saved/0"_q
		: u"/v1/channel-tabs/%1"_q.arg(ChatId(peer));
}

void Load(not_null<PeerData*> channel, Fn<void()> done = nullptr) {
	const auto key = ChannelKey(channel);
	Auth::EnsureToken(&channel->session(), [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(
			TabPath(channel),
			token,
			[=](QJsonObject result, int code) {
				if (code < 200 || code >= 300) {
					return;
				}
				G().states[key] = Parse(result);
				G().updates.fire_copy(key);
				if (done) {
					done();
				}
			});
	});
}

// The next page of cards; `path` gets the offset. A tab on a person's profile has no "state".
void LoadMoreFrom(
		not_null<Main::Session*> session,
		const QString &key,
		Fn<QString(int offset)> path,
		bool forceActive) {
	const auto state = Cached(key);
	if (!state || int(state->cards.size()) >= state->total
		|| G().loadingMore.contains(key)) {
		return;
	}
	G().loadingMore.emplace(key);
	const auto offset = int(state->cards.size());
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			G().loadingMore.remove(key);
			return;
		}
		Api::Get(path(offset), token, [=](QJsonObject result, int code) {
			G().loadingMore.remove(key);
			if (code < 200 || code >= 300) {
				return;
			}
			const auto more = Parse(result, forceActive);
			if (more->active && int(state->cards.size()) == offset) {
				state->cards.insert(
					end(state->cards),
					begin(more->cards),
					end(more->cards));
				G().updates.fire_copy(key);
			}
		});
	});
}

void LoadMore(not_null<PeerData*> channel) {
	const auto path = TabPath(channel);
	LoadMoreFrom(&channel->session(), ChannelKey(channel), [=](int offset) {
		return path + u"?offset=%1"_q.arg(offset);
	}, false);
}

using Done = Fn<void(QString error)>;

void Finish(const QJsonObject &result, int code, Done done) {
	if (code >= 200 && code < 300) {
		done(QString());
	} else {
		const auto detail = result.value(u"detail"_q).toString();
		done(detail.isEmpty() ? u"error"_q : detail);
	}
}

void WithToken(not_null<PeerData*> channel, Done done, Fn<void(QString)> run) {
	Auth::EnsureToken(&channel->session(), [=](QString token) {
		if (token.isEmpty()) {
			done(u"no_token"_q);
		} else {
			run(token);
		}
	});
}

void Attach(not_null<PeerData*> channel, QString bot, Done done) {
	WithToken(channel, done, [=](QString token) {
		Api::Put(TabPath(channel), QJsonObject{
			{ u"bot_username"_q, bot },
			{ u"chat_type"_q, ChatType(channel) },
		}, token, [=](QJsonObject result, int code) {
			Finish(result, code, done);
		});
	});
}

void Detach(not_null<PeerData*> channel, Done done) {
	WithToken(channel, done, [=](QString token) {
		Api::Delete(TabPath(channel), token, [=](QJsonObject result, int code) {
			Finish(result, code, done);
		});
	});
}

void Rename(not_null<PeerData*> channel, QString title, Done done) {
	WithToken(channel, done, [=](QString token) {
		auto body = QJsonObject();
		body.insert(
			u"title"_q,
			title.trimmed().isEmpty()
				? QJsonValue(QJsonValue::Null)
				: QJsonValue(title.trimmed()));
		Api::Put(TabPath(channel) + u"/title"_q, body, token, [=](
				QJsonObject result,
				int code) {
			Finish(result, code, done);
		});
	});
}

void Report(not_null<PeerData*> channel, QString reason, Done done) {
	WithToken(channel, done, [=](QString token) {
		Api::Post(TabPath(channel) + u"/report"_q, QJsonObject{
			{ u"reason"_q, reason },
		}, token, [=](QJsonObject result, int code) {
			Finish(result, code, done);
		});
	});
}

[[nodiscard]] rpl::producer<std::shared_ptr<State>> StateValue(
		not_null<PeerData*> channel) {
	const auto key = ChannelKey(channel);
	return rpl::single(key) | rpl::then(
		G().updates.events() | rpl::filter(rpl::mappers::_1 == key)
	) | rpl::map([=] {
		return Cached(key);
	});
}

[[nodiscard]] rpl::producer<QString> TitleValue(not_null<PeerData*> channel) {
	return StateValue(channel) | rpl::map([](std::shared_ptr<State> state) {
		return (state && state->active) ? state->title : QString();
	});
}

const QImage *Image(const QString &url) {
	if (url.isEmpty()) {
		return nullptr;
	}
	auto &g = G();
	if (const auto i = g.images.find(url); i != end(g.images)) {
		return &i->second;
	}
	if (g.imagesLoading.emplace(url).second) {
		Api::GetBytes(url, [=](QByteArray bytes, int code) {
			auto image = QImage::fromData(bytes);
			if (code == 200 && !image.isNull()) {
				G().images.emplace(url, std::move(image));
				G().imageLoaded.fire({});
			}
		});
	}
	return nullptr;
}

// Where a list of cards comes from, and what it does: a channel's tab or one on a person's profile.
struct Source {
	Fn<rpl::producer<std::shared_ptr<State>>()> state;
	Fn<void(std::shared_ptr<State>)> loadMore;
	Fn<void(not_null<Ui::PopupMenu*>, std::shared_ptr<State>)> fillMenu;
	Fn<void(std::shared_ptr<State>, const Card &)> open;
};

// The cards: square tiles three to a row, or rows when the bot chose a list.
class Cards final : public Ui::RpWidget {
public:
	Cards(QWidget *parent, Source source)
	: RpWidget(parent)
	, _source(std::move(source)) {
		setMouseTracking(true);
		_source.state() | rpl::on_next([=](std::shared_ptr<State> state) {
			_state = state;
			resizeToWidth(width());
			update();
		}, lifetime());
		G().imageLoaded.events() | rpl::on_next([=] {
			update();
		}, lifetime());
	}

	void setVisibleTopBottom(int top, int bottom) {
		_visibleTop = top;
		_visibleBottom = bottom;
		if (_state && bottom >= height() - rowHeight() * 3) {
			_source.loadMore(_state);
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto count = _state ? int(_state->cards.size()) : 0;
		if (!count && ShowsEmpty(_state) && newWidth > 0) {
			return emptyHeight(newWidth);
		}
		if (!count || newWidth <= 0) {
			return 0;
		}
		if (isList()) {
			return count * rowHeight();
		}
		const auto rows = (count + kColumns - 1) / kColumns;
		return rows * tileSize(newWidth);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		if (!_state) {
			return;
		}
		if (ShowsEmpty(_state)) {
			paintEmpty(p);
			return;
		}
		const auto clip = e->rect();
		for (auto i = 0; i != int(_state->cards.size()); ++i) {
			const auto rect = cardRect(i);
			if (!rect.intersects(clip)) {
				continue;
			}
			if (isList()) {
				paintRow(p, _state->cards[i], rect, i == _over);
			} else {
				paintTile(p, _state->cards[i], rect);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto index = cardAt(e->pos());
		if (_over != index) {
			_over = index;
			setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEventHook(QEvent *e) override {
		_over = -1;
		setCursor(style::cur_default);
		update();
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton || !_state) {
			return;
		}
		const auto index = cardAt(e->pos());
		if (index >= 0) {
			const auto state = _state;
			_source.open(state, state->cards[index]);
		}
	}

	void contextMenuEvent(QContextMenuEvent *e) override {
		if (!_state || !_state->active || !_source.fillMenu) {
			return;
		}
		_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
		_source.fillMenu(_menu.get(), _state);
		if (_menu->empty()) {
			_menu = nullptr;
			return;
		}
		_menu->popup(e->globalPos());
		e->accept();
	}

private:
	[[nodiscard]] bool isList() const {
		return _state && _state->list;
	}
	[[nodiscard]] int rowHeight() const {
		return isList() ? st::defaultPeerListItem.height : tileSize(width());
	}
	[[nodiscard]] static int tileSize(int width) {
		return std::max(width / kColumns, 1);
	}
	[[nodiscard]] QRect cardRect(int index) const {
		if (isList()) {
			return QRect(0, index * rowHeight(), width(), rowHeight());
		}
		const auto size = tileSize(width());
		return QRect(
			(index % kColumns) * size,
			(index / kColumns) * size,
			size,
			size);
	}
	[[nodiscard]] int cardAt(QPoint point) const {
		if (!_state) {
			return -1;
		}
		for (auto i = 0; i != int(_state->cards.size()); ++i) {
			if (cardRect(i).contains(point)) {
				return i;
			}
		}
		return -1;
	}

	// The bot's empty state for this viewer: its image above a line of text, centred, as Android's.
	[[nodiscard]] static int emptyPadding() {
		return style::ConvertScale(40);
	}
	[[nodiscard]] static int emptyImageSize() {
		return style::ConvertScale(120);
	}
	[[nodiscard]] QRect emptyTextRect(int width) const {
		const auto padding = emptyPadding();
		const auto top = padding
			+ (_state->emptyImage.isEmpty()
				? 0
				: (emptyImageSize() + style::ConvertScale(16)));
		const auto available = std::max(width - 2 * padding, 1);
		const auto height = _state->emptyText.isEmpty()
			? 0
			: QFontMetrics(st::normalFont->f).boundingRect(
				QRect(0, 0, available, 100000),
				Qt::TextWordWrap | Qt::AlignHCenter,
				_state->emptyText).height();
		return QRect(padding, top, available, height);
	}
	[[nodiscard]] int emptyHeight(int width) const {
		return emptyTextRect(width).y()
			+ emptyTextRect(width).height()
			+ emptyPadding();
	}
	void paintEmpty(QPainter &p) {
		if (!_state->emptyImage.isEmpty()) {
			const auto size = emptyImageSize();
			const auto rect = QRect((width() - size) / 2, emptyPadding(), size, size);
			if (const auto image = Image(_state->emptyImage)) {
				const auto ratio = style::DevicePixelRatio();
				const auto scaled = image->scaled(
					rect.size() * ratio,
					Qt::KeepAspectRatio,
					Qt::SmoothTransformation);
				const auto shown = QSize(scaled.width() / ratio, scaled.height() / ratio);
				auto hq = PainterHighQualityEnabler(p);
				auto path = QPainterPath();
				path.addRoundedRect(rect, style::ConvertScale(12), style::ConvertScale(12));
				p.save();
				p.setClipPath(path);
				p.drawImage(
					QRect(
						rect.x() + (size - shown.width()) / 2,
						rect.y() + (size - shown.height()) / 2,
						shown.width(),
						shown.height()),
					scaled);
				p.restore();
			}
		}
		if (!_state->emptyText.isEmpty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				emptyTextRect(width()),
				Qt::TextWordWrap | Qt::AlignHCenter,
				_state->emptyText);
		}
	}

	void paintCover(QPainter &p, const QImage &image, QRect rect) {
		const auto ratio = style::DevicePixelRatio();
		const auto scaled = image.scaled(
			rect.size() * ratio,
			Qt::KeepAspectRatioByExpanding,
			Qt::SmoothTransformation);
		const auto source = QRect(
			(scaled.width() - rect.width() * ratio) / 2,
			(scaled.height() - rect.height() * ratio) / 2,
			rect.width() * ratio,
			rect.height() * ratio);
		p.drawImage(rect, scaled, source);
	}

	void paintTile(QPainter &p, const Card &card, QRect rect) {
		const auto inner = rect.marginsRemoved({ 0, 0, 1, 1 });
		if (const auto image = Image(card.image)) {
			paintCover(p, *image, inner);
		} else {
			p.fillRect(inner, st::windowBgOver);
		}
		const auto shadeRect = QRect(
			inner.x(),
			inner.bottom() - st::normalFont->height * 3,
			inner.width(),
			st::normalFont->height * 3 + 1);
		auto gradient = QLinearGradient(0, shadeRect.top(), 0, shadeRect.bottom());
		gradient.setColorAt(0, QColor(0, 0, 0, 0));
		gradient.setColorAt(1, QColor(0, 0, 0, 160));
		p.fillRect(shadeRect, gradient);
		const auto skip = st::normalFont->height / 3;
		p.setFont(st::semiboldFont);
		p.setPen(QColor(255, 255, 255));
		p.drawText(
			QRect(
				inner.x() + skip,
				inner.bottom() - st::semiboldFont->height - skip,
				inner.width() - 2 * skip,
				st::semiboldFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::semiboldFont->elided(card.title, inner.width() - 2 * skip));
		if (!card.badge.isEmpty()) {
			const auto font = st::normalFont;
			const auto w = font->width(card.badge) + 2 * skip;
			const auto badge = QRect(inner.x() + skip, inner.y() + skip, w, font->height);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 150));
			p.drawRoundedRect(badge, skip, skip);
			p.setPen(QColor(255, 255, 255));
			p.setFont(font);
			p.drawText(badge, Qt::AlignCenter, card.badge);
		}
	}

	void paintRow(QPainter &p, const Card &card, QRect rect, bool over) {
		if (over) {
			p.fillRect(rect, st::windowBgOver);
		}
		const auto &item = st::defaultPeerListItem;
		const auto photo = item.photoSize;
		const auto left = item.photoPosition.x();
		const auto photoRect = QRect(
			rect.x() + left,
			rect.y() + (rect.height() - photo) / 2,
			photo,
			photo);
		if (const auto image = Image(card.image)) {
			auto path = QPainterPath();
			path.addRoundedRect(photoRect, st::roundRadiusLarge, st::roundRadiusLarge);
			p.save();
			p.setClipPath(path);
			paintCover(p, *image, photoRect);
			p.restore();
		}
		const auto textLeft = rect.x() + item.namePosition.x();
		auto right = rect.right() - left;
		if (!card.badge.isEmpty()) {
			p.setFont(st::semiboldFont);
			p.setPen(st::windowActiveTextFg);
			const auto w = st::semiboldFont->width(card.badge);
			p.drawText(
				QRect(right - w, rect.y(), w, rect.height()),
				Qt::AlignRight | Qt::AlignVCenter,
				card.badge);
			right -= w + left;
		}
		const auto available = std::max(right - textLeft, 1);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			textLeft,
			rect.y() + item.namePosition.y() + st::semiboldFont->ascent,
			st::semiboldFont->elided(card.title, available));
		if (!card.subtitle.isEmpty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				textLeft,
				rect.y() + item.statusPosition.y() + st::normalFont->ascent,
				st::normalFont->elided(card.subtitle, available));
		}
	}

	const Source _source;
	std::shared_ptr<State> _state;
	base::unique_qptr<Ui::PopupMenu> _menu;
	int _over = -1;
	int _visibleTop = 0;
	int _visibleBottom = 0;

};

// A viewer's complaint about a tab: the five reasons Telegram's own report asks for.
void AddReportActions(
		not_null<Ui::PopupMenu*> menu,
		Fn<void(QString reason, Done done)> report) {
	const auto reasons = std::vector<std::pair<QString, QString>>{
		{ u"spam"_q, tr::lng_report_reason_spam(tr::now) },
		{ u"violence"_q, tr::lng_report_reason_violence(tr::now) },
		{ u"porn"_q, tr::lng_report_reason_pornography(tr::now) },
		{ u"fake"_q, tr::lng_report_reason_fake(tr::now) },
		{ u"other"_q, tr::lng_report_reason_other(tr::now) },
	};
	for (const auto &[reason, label] : reasons) {
		menu->addAction(Tr(Str::BotTabReport) + u": "_q + label, [=] {
			report(reason, [](QString error) {
				if (error.isEmpty()) {
					Ui::Toast::Show(Tr(Str::BotTabReported));
				}
			});
		}, &st::menuIconReport);
	}
}

// The card about to open this bot's mini app: where it was, for svipeInitData (TakeOpenedFrom).
void RememberOpened(uint64 botId, PeerId peer, const QString &chatType) {
	if (botId) {
		G().openedFrom[botId] = { peer, chatType, crl::now() };
	}
}

[[nodiscard]] Source ChannelSource(not_null<PeerData*> channel) {
	return {
		.state = [=] { return StateValue(channel); },
		.loadMore = [=](std::shared_ptr<State>) { LoadMore(channel); },
		.fillMenu = [=](not_null<Ui::PopupMenu*> menu, std::shared_ptr<State>) {
			AddReportActions(menu, [=](QString reason, Done done) {
				Report(channel, reason, done);
			});
		},
		.open = [=](std::shared_ptr<State> state, const Card &card) {
			// The mini app is told which chat this is (chat_type "channel", "supergroup", "group").
			RememberOpened(state->botId, channel->id, ChatType(channel));
			UrlClickHandler::Open(card.link);
		},
	};
}

class TabAdapter final : public Info::Profile::MediaTabContent {
public:
	TabAdapter(Info::Profile::MediaTabContext context, Source source)
	: _source(source)
	, _cards(context.parent, std::move(source)) {
	}

	not_null<Ui::RpWidget*> widget() override {
		return _cards.data();
	}
	Info::Profile::TabTopBarBindings topBarBindings() override {
		return {
			.title = _source.state() | rpl::map([](std::shared_ptr<State> state) {
				return TextWithEntities{ (state && state->active) ? state->title : QString() };
			}),
		};
	}
	void resizeToWidth(int newWidth) override {
		_cards->resizeToWidth(newWidth);
	}
	void setVisibleRegion(int top, int bottom) override {
		_cards->setVisibleTopBottom(top, bottom);
	}

private:
	const Source _source;
	object_ptr<Cards> _cards;

};

// A tab in mode "web": the bot's page as tall as the tab (svipe_web_tab.h).
class WebAdapter final : public Info::Profile::MediaTabContent {
public:
	WebAdapter(
		Info::Profile::MediaTabContext context,
		Source source,
		WebTab::Args args)
	: _source(source)
	, _view(
		context.parent,
		&context.peer->session(),
		std::move(args)) {
	}

	not_null<Ui::RpWidget*> widget() override {
		return _view.data();
	}
	Info::Profile::TabTopBarBindings topBarBindings() override {
		return {
			.title = _source.state() | rpl::map([](std::shared_ptr<State> state) {
				return TextWithEntities{ (state && state->active) ? state->title : QString() };
			}),
		};
	}
	void resizeToWidth(int newWidth) override {
		_view->resizeToWidth(newWidth);
	}
	void setVisibleRegion(int top, int bottom) override {
		_view->setVisibleRegion(top, bottom);
	}
	void setTopOverlay(int height) override {
		_view->setTopOverlay(height);
	}

private:
	const Source _source;
	object_ptr<WebTab::View> _view;

};

// The tab's content for this state: the bot's page in mode "web", its cards otherwise.
[[nodiscard]] std::unique_ptr<Info::Profile::MediaTabContent> MakeContent(
		Info::Profile::MediaTabContext context,
		Source source,
		const std::shared_ptr<State> &state,
		PeerId where,
		const QString &chatType) {
	if (IsWeb(state)) {
		return std::make_unique<WebAdapter>(
			std::move(context),
			std::move(source),
			WebTab::Args{
				.botId = state->botId,
				.botUsername = state->botUsername,
				.url = state->webUrl,
				.peer = where,
				.chatType = chatType,
			});
	}
	return std::make_unique<TabAdapter>(std::move(context), std::move(source));
}

void ShowCardsBox(
		not_null<Window::SessionController*> window,
		Source source) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(source.state() | rpl::map([](std::shared_ptr<State> state) {
			return (state && state->active) ? state->title : QString();
		}));
		box->setWidth(st::boxWideWidth);
		const auto cards = box->addRow(
			object_ptr<Cards>(box, source),
			QMargins());
		const auto update = [=] {
			const auto top = box->scrollTop() - cards->y();
			cards->setVisibleTopBottom(top, top + box->scrollHeight());
		};
		box->scrolls() | rpl::on_next(update, cards->lifetime());
		cards->heightValue() | rpl::on_next([=](int) {
			update();
		}, cards->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

[[nodiscard]] QString StatusText(const std::shared_ptr<State> &state) {
	if (state && state->active) {
		return Tr(Str::BotTabActive).arg(state->title, u"@"_q + state->botUsername);
	}
	if (state && !state->myRequestBot.isEmpty()) {
		if (state->myRequestState == u"pending"_q) {
			return Tr(Str::BotTabPending).arg(u"@"_q + state->myRequestBot);
		} else if (state->myRequestState == u"refused"_q) {
			return Tr(Str::BotTabRefused).arg(u"@"_q + state->myRequestBot);
		}
	}
	return Tr(Str::BotTabNone);
}

[[nodiscard]] QString ErrorText(const QString &error) {
	if (error == u"bot_not_registered"_q) {
		return Tr(Str::BotTabErrorNotRegistered);
	} else if (error == u"bot_refused"_q) {
		return Tr(Str::BotTabErrorRefused);
	} else if (error == u"rename_not_allowed"_q) {
		return Tr(Str::BotTabErrorNoRename);
	} else if (error == u"not_attacher"_q) {
		return Tr(Str::BotTabErrorNotAllowed);
	}
	return tr::lng_attach_failed(tr::now);
}

void FillSettings(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> channel) {
	const auto type = ChatType(channel);
	const auto content = box->verticalLayout();
	content->clear();
	const auto state = Cached(channel);
	const auto refill = crl::guard(box, [=] {
		Load(channel, crl::guard(box, [=] {
			FillSettings(box, show, channel);
		}));
	});
	const auto run = [=](Fn<void(Done)> action) {
		action([=](QString error) {
			if (!error.isEmpty()) {
				show->showToast(ErrorText(error));
			}
			refill();
		});
	};

	Ui::AddSubsectionTitle(content, TrValue((type == u"saved"_q)
		? Str::BotTabCurrentSaved
		: (type == u"channel"_q)
		? Str::BotTabCurrent
		: Str::BotTabCurrentGroup));
	content->add(
		object_ptr<Ui::FlatLabel>(content, StatusText(state), st::boxLabel),
		st::boxRowPadding);
	const auto active = state && state->active;
	if (active && state->canRename) {
		const auto title = content->add(
			object_ptr<Ui::InputField>(
				content,
				st::defaultInputField,
				TrValue(Str::BotTabTitleHint),
				state->title),
			st::boxRowPadding);
		title->setMaxLength(16);
		Ui::AddSkip(content);
		content->add(object_ptr<Ui::SettingsButton>(
			content,
			TrValue(Str::BotTabRename),
			st::settingsButtonNoIcon)
		)->addClickHandler([=] {
			run([=](Done done) { Rename(channel, title->getLastText(), done); });
		});
	}
	if (active && state->isAttacher) {
		content->add(object_ptr<Ui::SettingsButton>(
			content,
			TrValue(Str::BotTabRemove),
			st::settingsAttentionButton)
		)->addClickHandler([=] {
			run([=](Done done) { Detach(channel, done); });
		});
	}
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSubsectionTitle(
		content,
		TrValue(active ? Str::BotTabReplace : Str::BotTabAttach));
	const auto username = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			TrValue(Str::BotTabUsernameHint)),
		st::boxRowPadding);
	Ui::AddSkip(content);
	content->add(object_ptr<Ui::SettingsButton>(
		content,
		TrValue(Str::BotTabSend),
		st::settingsButtonNoIcon)
	)->addClickHandler([=] {
		auto name = username->getLastText().trimmed();
		if (name.startsWith('@')) {
			name = name.mid(1);
		}
		if (name.isEmpty()) {
			username->showError();
			return;
		}
		run([=](Done done) { Attach(channel, name, done); });
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(content, TrValue((type == u"saved"_q)
		? Str::BotTabInfoSaved
		: (type == u"channel"_q)
		? Str::BotTabInfo
		: Str::BotTabInfoGroup));
	content->resizeToWidth(st::boxWideWidth);
}

// ---------------------------------------------------------------------------
// Tabs on a profile (app/api/profile_tabs.py, svipe.uz/bots/profile-tabs#profiles, #private;
// Android: SvipeProfileTabs). A registered bot asks, the person agrees, the tab is live at once: at most
// three of each kind, a fourth only in place of one the person picks. Kind "profile": on their profile,
// to everyone or to them only. Kind "private": on their profile to everyone, and to them on the profile
// of each of the bot's other users. Saved Messages and a bot's own profile have theirs too.

[[nodiscard]] uint64 Bare(not_null<UserData*> user) {
	return peerToUser(user->id).bare;
}

[[nodiscard]] QString ViewTag(ProfileView view) {
	switch (view) {
	case ProfileView::SavedMessages: return u"s"_q;
	case ProfileView::Bot: return u"b"_q;
	case ProfileView::Profile: break;
	}
	return u"p"_q;
}

[[nodiscard]] QString ProfilePrefix(uint64 owner, ProfileView view) {
	return u"u%1%2_"_q.arg(owner).arg(ViewTag(view));
}

[[nodiscard]] QString UserKey(
		uint64 owner,
		ProfileView view,
		uint64 bot,
		const QString &place) {
	return ProfilePrefix(owner, view) + u"%1_%2"_q.arg(bot).arg(place);
}

[[nodiscard]] QString KindOf(const QString &kind) {
	return (kind == u"private"_q) ? kind : u"profile"_q;
}

[[nodiscard]] std::vector<MyTab> ParseTabs(const QJsonObject &res) {
	auto result = std::vector<MyTab>();
	for (const auto &value : res.value(u"tabs"_q).toArray()) {
		const auto o = value.toObject();
		result.push_back({
			.botId = uint64(o.value(u"bot_id"_q).toVariant().toLongLong()),
			.username = OptString(o, u"username"_q),
			.title = OptString(o, u"title"_q),
			.everyone = (o.value(u"visibility"_q).toString() != u"me"_q),
			.kind = KindOf(OptString(o, u"kind"_q)),
			.place = OptString(o, u"place"_q),
		});
	}
	return result;
}

void SetMine(const QJsonObject &res) {
	G().mine = ParseTabs(res);
	G().mineLoaded = true;
}

[[nodiscard]] const MyTab *Mine(uint64 botId, const QString &kind) {
	for (const auto &tab : G().mine) {
		if (tab.botId == botId && tab.kind == kind) {
			return &tab;
		}
	}
	return nullptr;
}

void LoadMine(not_null<Main::Session*> session, Fn<void()> done) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			if (done) done();
			return;
		}
		Api::Get(u"/v1/profile-tabs"_q, token, [=](QJsonObject result, int code) {
			if (code == 200) {
				SetMine(result);
			}
			if (done) done();
		});
	});
}

[[nodiscard]] QString DeclinedKey(uint64 botId, const QString &kind) {
	return u"%1:%2"_q.arg(botId).arg(kind);
}

void Status(
		not_null<Main::Session*> session,
		uint64 botId,
		const QString &kind,
		Fn<void(QString)> done) {
	const auto answer = [=] {
		if (Mine(botId, kind)) {
			done(u"added"_q);
		} else {
			done(G().declined.contains(DeclinedKey(botId, kind))
				? u"declined"_q
				: u"missed"_q);
		}
	};
	if (G().mineLoaded) {
		answer();
	} else {
		LoadMine(session, answer);
	}
}

void FireProfile(uint64 owner, ProfileView view) {
	G().updates.fire(ProfilePrefix(owner, view));
}

[[nodiscard]] QString ListQuery(ProfileView view) {
	switch (view) {
	case ProfileView::SavedMessages: return u"?saved=1"_q;
	case ProfileView::Bot: return u"?bot=1"_q;
	case ProfileView::Profile: break;
	}
	return QString();
}

// The tabs this profile screen shows me, each with its first page of cards. A tab the bot hides from
// me (live cards: nothing for the two of us) answers 404 and isn't shown.
void LoadProfile(
		not_null<Main::Session*> session,
		uint64 owner,
		ProfileView view) {
	const auto prefix = ProfilePrefix(owner, view);
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(u"/v1/users/%1/tabs"_q.arg(owner) + ListQuery(view), token, [=](
				QJsonObject result,
				int code) {
			if (code != 200) {
				return;
			}
			auto tabs = std::vector<Shown>();
			for (const auto &tab : ParseTabs(result)) {
				tabs.push_back({
					tab.botId,
					tab.place.isEmpty() ? u"profile"_q : tab.place,
				});
			}
			if (tabs.empty()) {
				G().profiles[prefix] = {};
				FireProfile(owner, view);
				return;
			}
			const auto left = std::make_shared<int>(int(tabs.size()));
			for (const auto &tab : tabs) {
				const auto key = UserKey(owner, view, tab.botId, tab.place);
				Api::Get(
					u"/v1/users/%1/tabs/%2?place=%3"_q
						.arg(owner)
						.arg(tab.botId)
						.arg(tab.place),
					token,
					[=](QJsonObject result, int code) {
						if (code == 200) {
							auto state = Parse(result, true);
							state->ownerId = owner;
							state->botId = tab.botId;
							state->place = tab.place;
							G().states[key] = std::move(state);
						} else {
							G().states.remove(key);
						}
						if (--*left == 0) {
							auto shown = std::vector<Shown>();
							for (const auto &one : tabs) {
								if (G().states.contains(
										UserKey(owner, view, one.botId, one.place))) {
									shown.push_back(one);
								}
							}
							G().profiles[prefix] = std::move(shown);
							FireProfile(owner, view);
						}
					});
			}
		});
	});
}

[[nodiscard]] std::shared_ptr<State> Slot(
		uint64 owner,
		ProfileView view,
		int index) {
	const auto i = G().profiles.find(ProfilePrefix(owner, view));
	if (i == end(G().profiles) || index >= int(i->second.size())) {
		return nullptr;
	}
	const auto &tab = i->second[index];
	return Cached(UserKey(owner, view, tab.botId, tab.place));
}

[[nodiscard]] rpl::producer<std::shared_ptr<State>> SlotValue(
		uint64 owner,
		ProfileView view,
		int index) {
	const auto prefix = ProfilePrefix(owner, view);
	return rpl::single(QString()) | rpl::then(
		G().updates.events() | rpl::filter([=](const QString &key) {
			return key.startsWith(prefix);
		})
	) | rpl::map([=] {
		return Slot(owner, view, index);
	});
}

void WithSessionToken(
		not_null<Main::Session*> session,
		Done done,
		Fn<void(QString)> run) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			done(u"no_token"_q);
		} else {
			run(token);
		}
	});
}

// Takes my tab off; `owner` and `view`: the profile screen to refresh.
void RemoveMine(
		not_null<Main::Session*> session,
		uint64 botId,
		uint64 owner,
		ProfileView view) {
	const auto self = session->userId().bare;
	WithSessionToken(session, [](QString) {}, [=](QString token) {
		Api::Delete(u"/v1/profile-tabs/%1"_q.arg(botId), token, [=](
				QJsonObject result,
				int code) {
			if (code == 200) {
				SetMine(result);
			}
			LoadProfile(session, self, ProfileView::Profile);
			if (owner != self || view != ProfileView::Profile) {
				LoadProfile(session, owner, view);
			}
		});
	});
}

void RemoveSaved(not_null<Main::Session*> session) {
	const auto self = session->userId().bare;
	WithSessionToken(session, [](QString) {}, [=](QString token) {
		Api::Delete(u"/v1/chat-tabs/saved/0"_q, token, [=](QJsonObject, int) {
			G().states.remove(u"s"_q);
			LoadProfile(session, self, ProfileView::SavedMessages);
		});
	});
}

void SetEveryone(not_null<Main::Session*> session, uint64 botId, bool everyone) {
	WithSessionToken(session, [](QString) {}, [=](QString token) {
		Api::Patch(u"/v1/profile-tabs/%1"_q.arg(botId), QJsonObject{
			{ u"visibility"_q, everyone ? u"everyone"_q : u"me"_q },
		}, token, [=](QJsonObject result, int code) {
			if (code == 200) {
				SetMine(result);
			}
		});
	});
}

void ReportProfileTab(
		not_null<Main::Session*> session,
		std::shared_ptr<State> state,
		QString reason,
		Done done) {
	WithSessionToken(session, done, [=](QString token) {
		Api::Post(
			u"/v1/users/%1/tabs/%2/report"_q.arg(state->ownerId).arg(state->botId),
			QJsonObject{ { u"reason"_q, reason } },
			token,
			[=](QJsonObject result, int code) { Finish(result, code, done); });
	});
}

[[nodiscard]] Source ProfileSource(
		not_null<UserData*> owner,
		ProfileView view,
		int index) {
	const auto session = &owner->session();
	const auto ownerId = Bare(owner);
	return {
		.state = [=] { return SlotValue(ownerId, view, index); },
		.loadMore = [=](std::shared_ptr<State> state) {
			if (!state) {
				return;
			}
			const auto bot = state->botId;
			const auto place = state->place;
			LoadMoreFrom(session, UserKey(ownerId, view, bot, place), [=](int offset) {
				return u"/v1/users/%1/tabs/%2?place=%3&offset=%4"_q
					.arg(ownerId)
					.arg(bot)
					.arg(place)
					.arg(offset);
			}, true);
		},
		.fillMenu = [=](
				not_null<Ui::PopupMenu*> menu,
				std::shared_ptr<State> state) {
			const auto bot = state->botId;
			if (state->place == u"saved"_q) {
				// Saved Messages' tab is mine alone: take it off.
				menu->addAction(
					Tr(Str::BotTabRemove),
					[=] { RemoveSaved(session); },
					&st::menuIconDeleteAttention);
			} else if (state->place == u"private"_q && Mine(bot, u"private"_q)) {
				// My private tab, on my profile or on one of the bot's other users'.
				menu->addAction(
					Tr(Str::ProfileTabRemove),
					[=] { RemoveMine(session, bot, ownerId, view); },
					&st::menuIconDeleteAttention);
			} else if (state->place == u"profile"_q && owner->isSelf()) {
				// My own profile: who sees it, or take it off.
				const auto mine = Mine(bot, u"profile"_q);
				const auto everyone = !mine || mine->everyone;
				menu->addAction(
					Tr(Str::ProfileTabEveryone),
					[=] { SetEveryone(session, bot, true); },
					everyone ? &st::mediaPlayerMenuCheck : nullptr);
				menu->addAction(
					Tr(Str::ProfileTabOnlyMe),
					[=] { SetEveryone(session, bot, false); },
					everyone ? nullptr : &st::mediaPlayerMenuCheck);
				menu->addAction(
					Tr(Str::ProfileTabRemove),
					[=] { RemoveMine(session, bot, ownerId, view); },
					&st::menuIconDeleteAttention);
			} else {
				AddReportActions(menu, [=](QString reason, Done done) {
					ReportProfileTab(session, state, reason, done);
				});
			}
		},
		.open = [=](std::shared_ptr<State> state, const Card &card) {
			// The mini app it opens is told whose profile this is, and where the tab is: chat_type
			// "profile", "private", "saved" or "bot".
			RememberOpened(
				state->botId,
				owner->id,
				state->chatType.isEmpty() ? state->place : state->chatType);
			UrlClickHandler::Open(card.link);
		},
	};
}

[[nodiscard]] TextWithEntities WithBold(QString text, const QString &arg, const QString &name) {
	const auto at = text.indexOf(arg);
	if (at < 0) {
		return TextWithEntities{ text };
	}
	auto result = TextWithEntities{ text.left(at) };
	result.append(tr::bold(name));
	result.append(text.mid(at + arg.size()));
	return result;
}

[[nodiscard]] bool IsPrivate(const QString &kind) {
	return (kind == u"private"_q);
}

void AddProfileTab(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		const QString &kind,
		uint64 replace,
		Fn<void(bool shownNow, bool added)> done) {
	const auto session = &bot->session();
	const auto self = session->userId().bare;
	WithSessionToken(session, [=](QString) { done(true, false); }, [=](QString token) {
		auto body = QJsonObject{
			{ u"bot_username"_q, bot->username() },
			{ u"kind"_q, kind },
		};
		if (replace) {
			body.insert(u"replace_bot_id"_q, qint64(replace));
		}
		Api::Post(u"/v1/profile-tabs"_q, body, token, [=](QJsonObject result, int code) {
			if (code == 200) {
				SetMine(result);
				G().declined.remove(DeclinedKey(Bare(bot), kind));
				LoadProfile(session, self, ProfileView::Profile);
				if (show->valid()) {
					show->showToast(WithBold(
						Tr(IsPrivate(kind)
							? Str::ProfileTabAddedPrivate
							: Str::ProfileTabAdded),
						u"%1"_q,
						bot->name()));
				}
				done(true, true);
			} else {
				if (show->valid()) {
					show->showToast((result.value(u"detail"_q).toString() == u"bot_not_registered"_q)
						? Tr(Str::ProfileTabNotRegistered)
						: tr::lng_attach_failed(tr::now));
				}
				done(true, false);
			}
		});
	});
}

void AskBox(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		const QString &kind,
		Fn<void(bool shownNow, bool added)> done) {
	const auto answered = std::make_shared<bool>(false);
	const auto decline = [=] {
		G().declined.emplace(DeclinedKey(Bare(bot), kind));
		done(true, false);
	};
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		Ui::ConfirmBox(box, {
			.text = WithBold(
				Tr(IsPrivate(kind)
					? Str::ProfileTabRequestPrivate
					: Str::ProfileTabRequest),
				u"%1"_q,
				bot->name()),
			.confirmed = [=](Fn<void()> close) {
				if (!std::exchange(*answered, true)) {
					AddProfileTab(show, bot, kind, 0, done);
				}
				close();
			},
			.cancelled = [=](Fn<void()> close) {
				if (!std::exchange(*answered, true)) {
					decline();
				}
				close();
			},
			.confirmText = Tr(Str::ProfileTabAdd),
		});
		box->boxClosing() | rpl::on_next([=] {
			if (!std::exchange(*answered, true)) {
				decline();
			}
		}, box->lifetime());
	}));
}

[[nodiscard]] std::vector<MyTab> MineOf(const QString &kind) {
	auto result = std::vector<MyTab>();
	for (const auto &tab : G().mine) {
		if (tab.kind == kind) {
			result.push_back(tab);
		}
	}
	return result;
}

// Three of this kind already: which one this replaces, or cancel. Choosing is agreeing.
void ChooseBox(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		const QString &kind,
		Fn<void(bool shownNow, bool added)> done) {
	const auto mine = MineOf(kind);
	const auto answered = std::make_shared<bool>(false);
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TrValue(Str::ProfileTabFullTitle));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(WithBold(
				Tr(IsPrivate(kind)
					? Str::ProfileTabFullTextPrivate
					: Str::ProfileTabFullText).arg(kMaxProfileTabs),
				u"%2"_q,
				bot->name())),
			st::boxLabel));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(
			int(mine.size()) - 1);
		const auto padding = st::boxRowPadding
			+ QMargins(0, st::boxOptionListSkip, 0, 0);
		for (auto i = 0; i != int(mine.size()); ++i) {
			box->addRow(
				object_ptr<Ui::Radiobutton>(
					box,
					group,
					i,
					mine[i].title.isEmpty() ? mine[i].username : mine[i].title,
					st::defaultBoxCheckbox),
				padding);
		}
		box->addButton(TrValue(Str::ProfileTabReplace), [=] {
			if (!std::exchange(*answered, true)) {
				const auto index = std::clamp(group->current(), 0, int(mine.size()) - 1);
				AddProfileTab(show, bot, kind, mine[index].botId, done);
			}
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->boxClosing() | rpl::on_next([=] {
			if (!std::exchange(*answered, true)) {
				done(true, false);
			}
		}, box->lifetime());
	}));
}

} // namespace

Info::Profile::MediaTabDescriptor MakeTabDescriptor(
		not_null<PeerData*> peer) {
	Load(peer);
	return {
		.id = u"svipe_bot_tab"_q,
		.title = TitleValue(peer) | rpl::map([](QString text) {
			return TextWithEntities{ text };
		}),
		.shown = StateValue(peer) | rpl::map([](std::shared_ptr<State> state) {
			return state
				&& state->active
				&& (!state->cards.empty() || ShowsEmpty(state) || IsWeb(state));
		}),
		.factory = [=](Info::Profile::MediaTabContext context) {
			return MakeContent(
				std::move(context),
				ChannelSource(peer),
				Cached(peer),
				peer->id,
				ChatType(peer));
		},
	};
}

void AddClassicButton(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer,
		Ui::MultiSlideTracker &tracker,
		const style::icon &icon) {
	Load(peer);
	const auto titles = std::make_shared<QString>();
	const auto wrap = Info::Media::AddCountedButton(
		parent,
		StateValue(peer) | rpl::map([=](std::shared_ptr<State> state) {
			*titles = (state && state->active) ? state->title : QString();
			return (state && state->active)
				? std::max(state->total, ShowsEmpty(state) ? 1 : 0)
				: 0;
		}),
		[=](int count) {
			const auto state = Cached(peer);
			return *titles + u" · "_q + QString::number(state ? state->total : count);
		},
		tracker);
	object_ptr<Info::Profile::FloatingIcon>(
		wrap->entity(),
		icon,
		st::infoSharedMediaButtonIconPosition);
	wrap->entity()->addClickHandler([=] {
		ShowCardsBox(window, ChannelSource(peer));
	});
}

void ShowSettingsBox(
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TrValue(Str::BotTab));
		box->setWidth(st::boxWideWidth);
		FillSettings(box, show, peer);
		Load(peer, crl::guard(box, [=] {
			FillSettings(box, show, peer);
		}));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		if (peer->isSelf()) {
			// Saved Messages' tab shows in its profile screen: refresh it when the box closes.
			box->boxClosing() | rpl::on_next([=] {
				LoadProfile(
					&peer->session(),
					peer->session().userId().bare,
					ProfileView::SavedMessages);
			}, box->lifetime());
		}
	}));
}

Info::Profile::MediaTabDescriptor MakeProfileTabDescriptor(
		not_null<UserData*> user,
		int index,
		ProfileView view) {
	if (index == 0) {
		LoadProfile(&user->session(), Bare(user), view);
	}
	const auto owner = Bare(user);
	return {
		.id = u"svipe_profile_tab_%1%2"_q.arg(ViewTag(view)).arg(index),
		.title = SlotValue(owner, view, index) | rpl::map([](std::shared_ptr<State> state) {
			return TextWithEntities{ state ? state->title : QString() };
		}),
		.shown = SlotValue(owner, view, index) | rpl::map([](std::shared_ptr<State> state) {
			return state != nullptr;
		}),
		.factory = [=](Info::Profile::MediaTabContext context) {
			const auto state = Slot(owner, view, index);
			return MakeContent(
				std::move(context),
				ProfileSource(user, view, index),
				state,
				user->id,
				state
					? (state->chatType.isEmpty() ? state->place : state->chatType)
					: QString());
		},
	};
}

void AddProfileClassicButtons(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		Ui::MultiSlideTracker &tracker,
		const style::icon &icon,
		ProfileView view) {
	const auto owner = Bare(user);
	LoadProfile(&user->session(), owner, view);
	for (auto index = 0; index != kMaxShownTabs; ++index) {
		const auto titles = std::make_shared<QString>();
		const auto wrap = Info::Media::AddCountedButton(
			parent,
			SlotValue(owner, view, index) | rpl::map([=](std::shared_ptr<State> state) {
				*titles = state ? state->title : QString();
				// A tab with no cards yet still shows, so its owner can manage it.
				return state ? std::max(state->total, 1) : 0;
			}),
			[=](int) {
				const auto state = Slot(owner, view, index);
				return state
					? (*titles + u" · "_q + QString::number(state->total))
					: *titles;
			},
			tracker);
		object_ptr<Info::Profile::FloatingIcon>(
			wrap->entity(),
			icon,
			st::infoSharedMediaButtonIconPosition);
		wrap->entity()->addClickHandler([=] {
			ShowCardsBox(window, ProfileSource(user, view, index));
		});
	}
}

void RequestProfileTab(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		bool fromMiniApp,
		const QString &kind,
		Fn<void(bool shownNow, bool added)> done) {
	if (!bot->isBot()) {
		done(false, false);
		return;
	}
	const auto botId = Bare(bot);
	const auto which = KindOf(kind);
	Status(&bot->session(), botId, which, [=](QString status) {
		if (status == u"added"_q) {
			done(false, true);
		} else if ((fromMiniApp && status == u"declined"_q) || !show->valid()) {
			done(false, false);
		} else if (int(MineOf(which).size()) >= kMaxProfileTabs) {
			ChooseBox(show, bot, which, done);
		} else {
			AskBox(show, bot, which, done);
		}
	});
}

void RequestProfileTabByUsername(
		not_null<Window::SessionController*> controller,
		const QString &username,
		const QString &kind) {
	const auto session = &controller->session();
	const auto show = controller->uiShow();
	const auto ask = [=](PeerData *peer) {
		if (const auto bot = peer ? peer->asUser() : nullptr) {
			RequestProfileTab(show, bot, false, kind, [](bool, bool) {});
		}
	};
	if (const auto peer = session->data().peerByUsername(username)) {
		ask(peer);
		return;
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTP_string()
	)).done(crl::guard(session, [=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		ask(session->data().peerLoaded(peerFromMTP(data.vpeer())));
	})).send();
}

void ProfileTabStatus(
		not_null<UserData*> bot,
		const QString &kind,
		Fn<void(QString)> done) {
	Status(&bot->session(), Bare(bot), KindOf(kind), std::move(done));
}

OpenedFrom TakeOpenedFrom(not_null<UserData*> bot) {
	const auto i = G().openedFrom.find(Bare(bot));
	if (i == end(G().openedFrom)) {
		return {};
	}
	const auto opened = i->second;
	G().openedFrom.erase(i);
	if (crl::now() - opened.when > kOpenedFromTimeout) {
		return {};
	}
	return {
		.peer = bot->session().data().peerLoaded(opened.peer),
		.chatType = opened.chatType,
	};
}

} // namespace Svipe::BotTab
