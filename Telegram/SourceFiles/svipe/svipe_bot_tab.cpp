/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_bot_tab.h"

#include "data/data_channel.h"
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
#include "ui/widgets/buttons.h"
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
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtGui/QLinearGradient>

namespace Svipe::BotTab {
namespace {

constexpr auto kColumns = 3;

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
};

struct Global {
	base::flat_map<ChannelId, std::shared_ptr<State>> states;
	base::flat_set<ChannelId> loadingMore;
	base::flat_map<QString, QImage> images;
	base::flat_set<QString> imagesLoading;
	rpl::event_stream<ChannelId> updates;
	rpl::event_stream<> imageLoaded;
};

Global &G() {
	static auto result = Global();
	return result;
}

[[nodiscard]] QString OptString(const QJsonObject &o, const QString &key) {
	const auto value = o.value(key);
	return value.isString() ? value.toString() : QString();
}

[[nodiscard]] std::shared_ptr<State> Parse(const QJsonObject &res) {
	auto state = std::make_shared<State>();
	state->active = (res.value(u"state"_q).toString() == u"active"_q);
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

[[nodiscard]] std::shared_ptr<State> Cached(not_null<ChannelData*> channel) {
	const auto i = G().states.find(peerToChannel(channel->id).bare);
	return (i != end(G().states)) ? i->second : nullptr;
}

void Load(not_null<ChannelData*> channel, Fn<void()> done = nullptr) {
	const auto id = peerToChannel(channel->id);
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
				G().states[id] = Parse(result);
				G().updates.fire_copy(id);
				if (done) {
					done();
				}
			});
	});
}

void LoadMore(not_null<ChannelData*> channel) {
	const auto id = peerToChannel(channel->id);
	const auto state = Cached(channel);
	if (!state || int(state->cards.size()) >= state->total
		|| G().loadingMore.contains(id)) {
		return;
	}
	G().loadingMore.emplace(id);
	const auto offset = int(state->cards.size());
	Auth::EnsureToken(&channel->session(), [=](QString token) {
		if (token.isEmpty()) {
			G().loadingMore.remove(id);
			return;
		}
		Api::Get(
			u"/v1/channel-tabs/%1?offset=%2"_q.arg(id.bare).arg(offset),
			token,
			[=](QJsonObject result, int code) {
				G().loadingMore.remove(id);
				if (code < 200 || code >= 300) {
					return;
				}
				const auto more = Parse(result);
				if (more->active && int(state->cards.size()) == offset) {
					state->cards.insert(
						end(state->cards),
						begin(more->cards),
						end(more->cards));
					G().updates.fire_copy(id);
				}
			});
	});
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
	const auto id = peerToChannel(channel->id);
	return rpl::single(id) | rpl::then(
		G().updates.events() | rpl::filter(rpl::mappers::_1 == id)
	) | rpl::map([=] {
		return Cached(channel);
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

// The cards: square tiles three to a row, or rows when the bot chose a list.
class Cards final : public Ui::RpWidget {
public:
	Cards(QWidget *parent, not_null<ChannelData*> channel)
	: RpWidget(parent)
	, _channel(channel) {
		setMouseTracking(true);
		StateValue(channel) | rpl::on_next([=](std::shared_ptr<State> state) {
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
			LoadMore(_channel);
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
			UrlClickHandler::Open(_state->cards[index].link);
		}
	}

	void contextMenuEvent(QContextMenuEvent *e) override {
		if (!_state || !_state->active) {
			return;
		}
		_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
		const auto reasons = std::vector<std::pair<QString, QString>>{
			{ u"spam"_q, tr::lng_report_reason_spam(tr::now) },
			{ u"violence"_q, tr::lng_report_reason_violence(tr::now) },
			{ u"porn"_q, tr::lng_report_reason_pornography(tr::now) },
			{ u"fake"_q, tr::lng_report_reason_fake(tr::now) },
			{ u"other"_q, tr::lng_report_reason_other(tr::now) },
		};
		const auto channel = _channel;
		for (const auto &[reason, label] : reasons) {
			_menu->addAction(Tr(Str::BotTabReport) + u": "_q + label, [=] {
				Report(channel, reason, [](QString error) {
					if (error.isEmpty()) {
						Ui::Toast::Show(Tr(Str::BotTabReported));
					}
				});
			}, &st::menuIconReport);
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

	const not_null<ChannelData*> _channel;
	std::shared_ptr<State> _state;
	base::unique_qptr<Ui::PopupMenu> _menu;
	int _over = -1;
	int _visibleTop = 0;
	int _visibleBottom = 0;

};

class TabAdapter final : public Info::Profile::MediaTabContent {
public:
	TabAdapter(
		Info::Profile::MediaTabContext context,
		not_null<ChannelData*> channel)
	: _channel(channel)
	, _cards(context.parent, channel) {
	}

	not_null<Ui::RpWidget*> widget() override {
		return _cards.data();
	}
	Info::Profile::TabTopBarBindings topBarBindings() override {
		return {
			.title = TitleValue(_channel) | rpl::map([](QString text) {
				return TextWithEntities{ text };
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
	const not_null<ChannelData*> _channel;
	object_ptr<Cards> _cards;

};

void ShowCardsBox(
		not_null<Window::SessionController*> window,
		not_null<ChannelData*> channel) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TitleValue(channel));
		box->setWidth(st::boxWideWidth);
		const auto cards = box->addRow(
			object_ptr<Cards>(box, channel),
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
			return std::make_unique<TabAdapter>(std::move(context), channel);
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
		ShowCardsBox(window, channel);
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

} // namespace Svipe::BotTab
