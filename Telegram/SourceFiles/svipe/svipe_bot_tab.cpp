/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bot_tab.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "info/info_controller.h"
#include "info/media/info_media_buttons.h"
#include "info/profile/info_profile_icon.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_auth.h"
#include "svipe/svipe_strings.h"
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
};

// States by key: "c<channel>" for a channel's tab, "u<owner>_<bot>" for a tab on a person's profile.
struct Global {
	base::flat_map<QString, std::shared_ptr<State>> states;
	base::flat_set<QString> loadingMore;
	base::flat_map<QString, QImage> images;
	base::flat_set<QString> imagesLoading;
	rpl::event_stream<QString> updates;
	rpl::event_stream<> imageLoaded;

	base::flat_map<uint64, std::vector<uint64>> profiles; // owner -> the bots whose tabs I see there
	std::vector<MyTab> mine;
	bool mineLoaded = false;
	base::flat_set<uint64> declined; // asked from a mini app and said no, this run
	base::flat_map<uint64, std::pair<uint64, crl::time>> openedFrom; // bot -> (owner, when)
};

Global &G() {
	static auto result = Global();
	return result;
}

[[nodiscard]] QString OptString(const QJsonObject &o, const QString &key) {
	const auto value = o.value(key);
	return value.isString() ? value.toString() : QString();
}

[[nodiscard]] QString ChannelKey(not_null<ChannelData*> channel) {
	return u"c%1"_q.arg(peerToChannel(channel->id).bare);
}

[[nodiscard]] QString UserKey(uint64 owner, uint64 bot) {
	return u"u%1_%2"_q.arg(owner).arg(bot);
}

[[nodiscard]] std::shared_ptr<State> Parse(
		const QJsonObject &res,
		bool forceActive = false) {
	auto state = std::make_shared<State>();
	state->active = forceActive
		|| (res.value(u"state"_q).toString() == u"active"_q);
	state->botUsername = OptString(res.value(u"bot"_q).toObject(), u"username"_q);
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

[[nodiscard]] std::shared_ptr<State> Cached(not_null<ChannelData*> channel) {
	return Cached(ChannelKey(channel));
}

void Load(not_null<ChannelData*> channel, Fn<void()> done = nullptr) {
	const auto id = peerToChannel(channel->id);
	const auto key = ChannelKey(channel);
	Auth::EnsureToken(&channel->session(), [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(
			u"/v1/channel-tabs/%1"_q.arg(id.bare),
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

void LoadMore(not_null<ChannelData*> channel) {
	const auto id = peerToChannel(channel->id).bare;
	LoadMoreFrom(&channel->session(), ChannelKey(channel), [=](int offset) {
		return u"/v1/channel-tabs/%1?offset=%2"_q.arg(id).arg(offset);
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

void WithToken(not_null<ChannelData*> channel, Done done, Fn<void(QString)> run) {
	Auth::EnsureToken(&channel->session(), [=](QString token) {
		if (token.isEmpty()) {
			done(u"no_token"_q);
		} else {
			run(token);
		}
	});
}

[[nodiscard]] QString TabPath(not_null<ChannelData*> channel) {
	return u"/v1/channel-tabs/%1"_q.arg(peerToChannel(channel->id).bare);
}

void Attach(not_null<ChannelData*> channel, QString bot, Done done) {
	WithToken(channel, done, [=](QString token) {
		Api::Put(TabPath(channel), QJsonObject{
			{ u"bot_username"_q, bot },
		}, token, [=](QJsonObject result, int code) {
			Finish(result, code, done);
		});
	});
}

void Detach(not_null<ChannelData*> channel, Done done) {
	WithToken(channel, done, [=](QString token) {
		Api::Delete(TabPath(channel), token, [=](QJsonObject result, int code) {
			Finish(result, code, done);
		});
	});
}

void Rename(not_null<ChannelData*> channel, QString title, Done done) {
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

void Report(not_null<ChannelData*> channel, QString reason, Done done) {
	WithToken(channel, done, [=](QString token) {
		Api::Post(TabPath(channel) + u"/report"_q, QJsonObject{
			{ u"reason"_q, reason },
		}, token, [=](QJsonObject result, int code) {
			Finish(result, code, done);
		});
	});
}

[[nodiscard]] rpl::producer<std::shared_ptr<State>> StateValue(
		not_null<ChannelData*> channel) {
	const auto key = ChannelKey(channel);
	return rpl::single(key) | rpl::then(
		G().updates.events() | rpl::filter(rpl::mappers::_1 == key)
	) | rpl::map([=] {
		return Cached(key);
	});
}

[[nodiscard]] rpl::producer<QString> TitleValue(not_null<ChannelData*> channel) {
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

[[nodiscard]] Source ChannelSource(not_null<ChannelData*> channel) {
	return {
		.state = [=] { return StateValue(channel); },
		.loadMore = [=](std::shared_ptr<State>) { LoadMore(channel); },
		.fillMenu = [=](not_null<Ui::PopupMenu*> menu, std::shared_ptr<State>) {
			AddReportActions(menu, [=](QString reason, Done done) {
				Report(channel, reason, done);
			});
		},
		.open = [](std::shared_ptr<State>, const Card &card) {
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
		not_null<ChannelData*> channel) {
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

	Ui::AddSubsectionTitle(content, TrValue(Str::BotTabCurrent));
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
	Ui::AddDividerText(content, TrValue(Str::BotTabInfo));
	content->resizeToWidth(st::boxWideWidth);
}

// ---------------------------------------------------------------------------
// Tabs on a person's own profile (app/api/profile_tabs.py, svipe.uz/bots/profile-tabs#profiles;
// Android: SvipeProfileTabs). A registered bot asks, the person agrees, the tab is live at once: at most
// three, a fourth only in place of one the person picks; each shown to everyone or to its owner only.

[[nodiscard]] uint64 Bare(not_null<UserData*> user) {
	return peerToUser(user->id).bare;
}

[[nodiscard]] std::vector<MyTab> ParseTabs(const QJsonObject &res) {
	auto result = std::vector<MyTab>();
	for (const auto &value : res.value(u"tabs"_q).toArray()) {
		const auto o = value.toObject();
		result.push_back({
			.botId = uint64(o.value(u"bot_id"_q).toInteger()),
			.username = OptString(o, u"username"_q),
			.title = OptString(o, u"title"_q),
			.everyone = (o.value(u"visibility"_q).toString() != u"me"_q),
		});
	}
	return result;
}

void SetMine(const QJsonObject &res) {
	G().mine = ParseTabs(res);
	G().mineLoaded = true;
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

void Status(
		not_null<Main::Session*> session,
		uint64 botId,
		Fn<void(QString)> done) {
	const auto answer = [=] {
		for (const auto &tab : G().mine) {
			if (tab.botId == botId) {
				done(u"added"_q);
				return;
			}
		}
		done(G().declined.contains(botId) ? u"declined"_q : u"missed"_q);
	};
	if (G().mineLoaded) {
		answer();
	} else {
		LoadMine(session, answer);
	}
}

void FireProfile(uint64 owner) {
	G().updates.fire(u"u%1_"_q.arg(owner));
}

// The tabs this person's profile shows me, each with its first page of cards.
void LoadProfile(not_null<Main::Session*> session, uint64 owner) {
	Auth::EnsureToken(session, [=](QString token) {
		if (token.isEmpty()) {
			return;
		}
		Api::Get(u"/v1/users/%1/tabs"_q.arg(owner), token, [=](
				QJsonObject result,
				int code) {
			if (code != 200) {
				return;
			}
			const auto tabs = ParseTabs(result);
			auto bots = std::vector<uint64>();
			for (const auto &tab : tabs) {
				bots.push_back(tab.botId);
			}
			if (bots.empty()) {
				G().profiles[owner] = {};
				FireProfile(owner);
				return;
			}
			const auto left = std::make_shared<int>(int(bots.size()));
			for (const auto bot : bots) {
				Api::Get(u"/v1/users/%1/tabs/%2"_q.arg(owner).arg(bot), token, [=](
						QJsonObject result,
						int code) {
					if (code == 200) {
						auto state = Parse(result, true);
						state->ownerId = owner;
						state->botId = bot;
						G().states[UserKey(owner, bot)] = std::move(state);
					} else {
						G().states.remove(UserKey(owner, bot));
					}
					if (--*left == 0) {
						auto shown = std::vector<uint64>();
						for (const auto id : bots) {
							if (G().states.contains(UserKey(owner, id))) {
								shown.push_back(id);
							}
						}
						G().profiles[owner] = std::move(shown);
						FireProfile(owner);
					}
				});
			}
		});
	});
}

[[nodiscard]] std::shared_ptr<State> Slot(uint64 owner, int index) {
	const auto i = G().profiles.find(owner);
	if (i == end(G().profiles) || index >= int(i->second.size())) {
		return nullptr;
	}
	return Cached(UserKey(owner, i->second[index]));
}

[[nodiscard]] rpl::producer<std::shared_ptr<State>> SlotValue(
		uint64 owner,
		int index) {
	const auto prefix = u"u%1_"_q.arg(owner);
	return rpl::single(QString()) | rpl::then(
		G().updates.events() | rpl::filter([=](const QString &key) {
			return key.startsWith(prefix);
		})
	) | rpl::map([=] {
		return Slot(owner, index);
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

void RemoveMine(not_null<Main::Session*> session, uint64 botId) {
	const auto self = session->userId().bare;
	WithSessionToken(session, [](QString) {}, [=](QString token) {
		Api::Delete(u"/v1/profile-tabs/%1"_q.arg(botId), token, [=](
				QJsonObject result,
				int code) {
			if (code == 200) {
				SetMine(result);
			}
			LoadProfile(session, self);
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

[[nodiscard]] Source ProfileSource(not_null<UserData*> owner, int index) {
	const auto session = &owner->session();
	const auto ownerId = Bare(owner);
	return {
		.state = [=] { return SlotValue(ownerId, index); },
		.loadMore = [=](std::shared_ptr<State> state) {
			if (!state) {
				return;
			}
			const auto bot = state->botId;
			LoadMoreFrom(session, UserKey(ownerId, bot), [=](int offset) {
				return u"/v1/users/%1/tabs/%2?offset=%3"_q
					.arg(ownerId)
					.arg(bot)
					.arg(offset);
			}, true);
		},
		.fillMenu = [=](
				not_null<Ui::PopupMenu*> menu,
				std::shared_ptr<State> state) {
			if (!owner->isSelf()) {
				AddReportActions(menu, [=](QString reason, Done done) {
					ReportProfileTab(session, state, reason, done);
				});
				return;
			}
			// My own profile: who sees it, or take it off.
			auto everyone = true;
			for (const auto &tab : G().mine) {
				if (tab.botId == state->botId) {
					everyone = tab.everyone;
				}
			}
			const auto bot = state->botId;
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
				[=] { RemoveMine(session, bot); },
				&st::menuIconDeleteAttention);
		},
		.open = [=](std::shared_ptr<State> state, const Card &card) {
			// The mini app it opens is told whose profile this is (chat_type "profile").
			G().openedFrom[state->botId] = { ownerId, crl::now() };
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

void AddProfileTab(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		uint64 replace,
		Fn<void(bool shownNow, bool added)> done) {
	const auto session = &bot->session();
	const auto self = session->userId().bare;
	WithSessionToken(session, [=](QString) { done(true, false); }, [=](QString token) {
		auto body = QJsonObject{ { u"bot_username"_q, bot->username() } };
		if (replace) {
			body.insert(u"replace_bot_id"_q, qint64(replace));
		}
		Api::Post(u"/v1/profile-tabs"_q, body, token, [=](QJsonObject result, int code) {
			if (code == 200) {
				SetMine(result);
				G().declined.remove(Bare(bot));
				LoadProfile(session, self);
				if (show->valid()) {
					show->showToast(WithBold(
						Tr(Str::ProfileTabAdded),
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
		Fn<void(bool shownNow, bool added)> done) {
	const auto answered = std::make_shared<bool>(false);
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		Ui::ConfirmBox(box, {
			.text = WithBold(Tr(Str::ProfileTabRequest), u"%1"_q, bot->name()),
			.confirmed = [=](Fn<void()> close) {
				if (!std::exchange(*answered, true)) {
					AddProfileTab(show, bot, 0, done);
				}
				close();
			},
			.cancelled = [=](Fn<void()> close) {
				if (!std::exchange(*answered, true)) {
					G().declined.emplace(Bare(bot));
					done(true, false);
				}
				close();
			},
			.confirmText = Tr(Str::ProfileTabAdd),
		});
		box->boxClosing() | rpl::on_next([=] {
			if (!std::exchange(*answered, true)) {
				G().declined.emplace(Bare(bot));
				done(true, false);
			}
		}, box->lifetime());
	}));
}

// Three already: which one this replaces, or cancel. Choosing is agreeing.
void ChooseBox(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		Fn<void(bool shownNow, bool added)> done) {
	const auto mine = G().mine;
	const auto answered = std::make_shared<bool>(false);
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TrValue(Str::ProfileTabFullTitle));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(WithBold(
				Tr(Str::ProfileTabFullText).arg(kMaxProfileTabs),
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
				AddProfileTab(show, bot, mine[index].botId, done);
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
		not_null<ChannelData*> channel) {
	Load(channel);
	return {
		.id = u"svipe_bot_tab"_q,
		.title = TitleValue(channel) | rpl::map([](QString text) {
			return TextWithEntities{ text };
		}),
		.shown = StateValue(channel) | rpl::map([](std::shared_ptr<State> state) {
			return state && state->active && !state->cards.empty();
		}),
		.factory = [=](Info::Profile::MediaTabContext context) {
			return std::make_unique<TabAdapter>(
				std::move(context),
				ChannelSource(channel));
		},
	};
}

void AddClassicButton(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> window,
		not_null<ChannelData*> channel,
		Ui::MultiSlideTracker &tracker,
		const style::icon &icon) {
	Load(channel);
	const auto titles = std::make_shared<QString>();
	const auto wrap = Info::Media::AddCountedButton(
		parent,
		StateValue(channel) | rpl::map([=](std::shared_ptr<State> state) {
			*titles = (state && state->active) ? state->title : QString();
			return (state && state->active) ? state->total : 0;
		}),
		[=](int count) {
			return *titles + u" · "_q + QString::number(count);
		},
		tracker);
	object_ptr<Info::Profile::FloatingIcon>(
		wrap->entity(),
		icon,
		st::infoSharedMediaButtonIconPosition);
	wrap->entity()->addClickHandler([=] {
		ShowCardsBox(window, ChannelSource(channel));
	});
}

void ShowSettingsBox(
		std::shared_ptr<Ui::Show> show,
		not_null<ChannelData*> channel) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TrValue(Str::BotTab));
		box->setWidth(st::boxWideWidth);
		FillSettings(box, show, channel);
		Load(channel, crl::guard(box, [=] {
			FillSettings(box, show, channel);
		}));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

Info::Profile::MediaTabDescriptor MakeProfileTabDescriptor(
		not_null<UserData*> user,
		int index) {
	if (index == 0) {
		LoadProfile(&user->session(), Bare(user));
	}
	const auto owner = Bare(user);
	return {
		.id = u"svipe_profile_tab_%1"_q.arg(index),
		.title = SlotValue(owner, index) | rpl::map([](std::shared_ptr<State> state) {
			return TextWithEntities{ state ? state->title : QString() };
		}),
		.shown = SlotValue(owner, index) | rpl::map([](std::shared_ptr<State> state) {
			return state != nullptr;
		}),
		.factory = [=](Info::Profile::MediaTabContext context) {
			return std::make_unique<TabAdapter>(
				std::move(context),
				ProfileSource(user, index));
		},
	};
}

void AddProfileClassicButtons(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		Ui::MultiSlideTracker &tracker,
		const style::icon &icon) {
	const auto owner = Bare(user);
	LoadProfile(&user->session(), owner);
	for (auto index = 0; index != kMaxProfileTabs; ++index) {
		const auto titles = std::make_shared<QString>();
		const auto wrap = Info::Media::AddCountedButton(
			parent,
			SlotValue(owner, index) | rpl::map([=](std::shared_ptr<State> state) {
				*titles = state ? state->title : QString();
				// A tab with no cards yet still shows, so its owner can manage it.
				return state ? std::max(state->total, 1) : 0;
			}),
			[=](int) {
				const auto state = Slot(owner, index);
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
			ShowCardsBox(window, ProfileSource(user, index));
		});
	}
}

void RequestProfileTab(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		bool fromMiniApp,
		Fn<void(bool shownNow, bool added)> done) {
	if (!bot->isBot()) {
		done(false, false);
		return;
	}
	const auto botId = Bare(bot);
	Status(&bot->session(), botId, [=](QString status) {
		if (status == u"added"_q) {
			done(false, true);
		} else if ((fromMiniApp && status == u"declined"_q) || !show->valid()) {
			done(false, false);
		} else if (int(G().mine.size()) >= kMaxProfileTabs) {
			ChooseBox(show, bot, done);
		} else {
			AskBox(show, bot, done);
		}
	});
}

void RequestProfileTabByUsername(
		not_null<Window::SessionController*> controller,
		const QString &username) {
	const auto session = &controller->session();
	const auto show = controller->uiShow();
	const auto ask = [=](PeerData *peer) {
		if (const auto bot = peer ? peer->asUser() : nullptr) {
			RequestProfileTab(show, bot, false, [](bool, bool) {});
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

void ProfileTabStatus(not_null<UserData*> bot, Fn<void(QString)> done) {
	Status(&bot->session(), Bare(bot), std::move(done));
}

UserData *TakeOpenedFromProfile(not_null<UserData*> bot) {
	const auto i = G().openedFrom.find(Bare(bot));
	if (i == end(G().openedFrom)) {
		return nullptr;
	}
	const auto [owner, when] = i->second;
	G().openedFrom.erase(i);
	if (crl::now() - when > kOpenedFromTimeout) {
		return nullptr;
	}
	return bot->session().data().userLoaded(UserId(owner));
}

} // namespace Svipe::BotTab
