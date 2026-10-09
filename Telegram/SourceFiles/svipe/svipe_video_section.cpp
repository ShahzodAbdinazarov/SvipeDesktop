/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_video_section.h"

#include "apiwrap.h"
#include "base/timer_rpl.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_message_reaction_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "lang/lang_tag.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_player.h"
#include "svipe/svipe_reels.h"
#include "svipe/svipe_reels_section.h"
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"
#include "svipe/svipe_video.h"
#include "ui/image/image.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/userpic_view.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/text/text_utilities.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include "window/main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

namespace Svipe::Video {
namespace {

// Android: SvipeExploreGrid long-form page size, and how close to the end the next page is asked.
constexpr auto kPageSize = 20;
constexpr auto kLoadMoreRows = 2;
constexpr auto kRelatedPageSize = 20;
constexpr auto kCheckPeriod = crl::time(250);
constexpr auto kLoaderPriority = 3;
const auto kLikeEmoji = QString::fromUtf8("\xe2\x9d\xa4");

[[nodiscard]] int S(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString DurationText(crl::time ms) {
	if (ms <= 0) {
		return QString();
	}
	const auto total = int(ms / 1000);
	const auto h = total / 3600, m = (total % 3600) / 60, s = total % 60;
	return h
		? u"%1:%2:%3"_q.arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
		: u"%1:%2"_q.arg(m).arg(s, 2, 10, QChar('0'));
}

// The poster of a wide card or a related row: Telegram's own thumbnail once the post is resolved,
// the stripped blur from the list JSON until then.
struct Card {
	std::shared_ptr<Item> item;
	std::shared_ptr<Data::DocumentMedia> media;
	Ui::PeerUserpicView userpic;
	QImage blur;
	Ui::Text::String title;
	bool titled = false;
	bool retitle = false;
	bool asked = false;
	bool upgraded = false;
};

// A card's post came from a link preview: once its channel is addressable, fetch the real message for
// the caption, the views and the date the preview does not carry.
void Upgrade(not_null<Reels::Feed*> feed, Card &card, QWidget *guard, Fn<void()> updated) {
	if (card.upgraded || !card.item->document || card.item->fullId) {
		return;
	}
	card.upgraded = true;
	const auto item = card.item;
	feed->upgradeToMessage(item, crl::guard(guard, [=] {
		updated();
	}));
}

[[nodiscard]] HistoryItem *MessageOf(
		not_null<Main::Session*> session,
		const Item &item) {
	return item.fullId ? session->data().message(item.fullId) : nullptr;
}

[[nodiscard]] ChannelData *ChannelOf(
		not_null<Main::Session*> session,
		const Item &item) {
	if (const auto message = MessageOf(session, item)) {
		return message->history()->peer->asChannel();
	}
	const auto channel = session->data().channelLoaded(ChannelId(item.channelId));
	return (channel && !channel->name().isEmpty()) ? channel : nullptr;
}

// SvipeWideVideoCell.metaLine: "Channel · N views · date".
[[nodiscard]] QString MetaLine(
		not_null<Main::Session*> session,
		const Item &item) {
	auto parts = QStringList();
	if (const auto channel = ChannelOf(session, item)) {
		parts.push_back(channel->name());
	} else {
		parts.push_back(u"@"_q + item.username);
	}
	if (const auto message = MessageOf(session, item)) {
		if (const auto views = message->viewsCount(); views > 0) {
			parts.push_back(tr::lng_stories_views(
				tr::now,
				lt_count_decimal,
				views));
		}
		if (const auto date = message->date(); date > 0) {
			parts.push_back(langDayOfMonthFull(base::unixtime::parse(date).date()));
		}
	}
	return parts.join(QString::fromUtf8("  \xC2\xB7  "));
}

void RefreshCard(Card &card) {
	if (!card.item->document || (card.titled && !card.retitle)) {
		return;
	}
	card.titled = true;
	card.retitle = false;
	const auto text = card.item->caption.text.trimmed();
	card.title.setText(st::semiboldTextStyle, text.section('\n', 0, 0));
	card.media = card.item->document->createMediaView();
	card.item->document->loadThumbnail(card.item->origin);
}

void PaintThumb(QPainter &p, Card &card, QRect rect, int radius, bool badge = true) {
	auto path = QPainterPath();
	path.addRoundedRect(rect, radius, radius);
	p.save();
	p.setClipPath(path);
	p.fillRect(rect, st::windowBgOver);
	auto image = QImage();
	if (card.media) {
		if (const auto thumb = card.media->thumbnail()) {
			image = thumb->original();
		} else if (const auto inline_ = card.media->thumbnailInline()) {
			image = inline_->original();
		}
	}
	if (image.isNull() && !card.item->inlineThumb.isEmpty()) {
		if (card.blur.isNull()) {
			card.blur = Images::FromInlineBytes(card.item->inlineThumb);
		}
		image = card.blur;
	}
	if (!image.isNull()) {
		// Cover the rect, the way YouTube crops a poster to 16:9.
		const auto size = image.size().scaled(rect.size(), Qt::KeepAspectRatioByExpanding);
		const auto target = QRect(
			rect.x() + (rect.width() - size.width()) / 2,
			rect.y() + (rect.height() - size.height()) / 2,
			size.width(),
			size.height());
		auto hq = PainterHighQualityEnabler(p);
		p.drawImage(target, image);
	}
	const auto duration = DurationText(card.item->document && card.item->document->hasDuration()
		? card.item->document->duration()
		: card.item->durationMs);
	if (badge && !duration.isEmpty()) {
		const auto font = st::semiboldFont;
		const auto w = font->width(duration) + S(10);
		const auto h = font->height + S(4);
		const auto badge = QRect(
			rect.x() + rect.width() - w - S(8),
			rect.y() + rect.height() - h - S(8),
			w,
			h);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 190));
		p.drawRoundedRect(badge, S(4), S(4));
		p.setFont(font);
		p.setPen(QColor(255, 255, 255));
		p.drawText(badge, Qt::AlignCenter, duration);
	}
	p.restore();
}

void PaintUserpic(
		Painter &p,
		not_null<Main::Session*> session,
		Card &card,
		int x,
		int y,
		int size) {
	if (const auto channel = ChannelOf(session, *card.item)) {
		channel->paintUserpic(p, card.userpic, x, y, size);
	} else {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawEllipse(QRect(x, y, size, size));
	}
}

// ---- The grid ------------------------------------------------------------------------------

class Grid final : public Ui::RpWidget {
public:
	Grid(
		QWidget *parent,
		not_null<Main::Session*> session,
		not_null<Reels::Feed*> feed,
		not_null<List*> list);

	[[nodiscard]] rpl::producer<std::shared_ptr<Item>> openRequests() const {
		return _openRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	struct Layout {
		int padding = 0;
		int gap = 0;
		int rowGap = 0;
		int columns = 1;
		int cardWidth = 0;
		int thumbHeight = 0;
		int rowHeight = 0;
	};
	void sync();
	[[nodiscard]] Layout layout(int width) const;
	[[nodiscard]] QRect cardRect(int index) const;
	[[nodiscard]] int cardAt(QPoint point) const;
	void resolveVisible();
	void retitle(const std::shared_ptr<Item> &item);

	const not_null<Main::Session*> _session;
	const not_null<Reels::Feed*> _feed;
	const not_null<List*> _list;
	std::vector<Card> _cards;
	Layout _layout;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<std::shared_ptr<Item>> _openRequests;

};

Grid::Grid(
	QWidget *parent,
	not_null<Main::Session*> session,
	not_null<Reels::Feed*> feed,
	not_null<List*> list)
: RpWidget(parent)
, _session(session)
, _feed(feed)
, _list(list) {
	setMouseTracking(true);
	_list->updates() | rpl::on_next([=] {
		sync();
	}, lifetime());
	sync();
	if (_list->items().empty()) {
		_list->loadMore();
	}
}

void Grid::sync() {
	const auto &items = _list->items();
	auto cards = std::vector<Card>();
	cards.reserve(items.size());
	for (const auto &item : items) {
		const auto i = ranges::find(_cards, item, &Card::item);
		if (i != end(_cards)) {
			cards.push_back(std::move(*i));
		} else {
			cards.push_back(Card{ .item = item });
		}
	}
	_cards = std::move(cards);
	resizeToWidth(width());
	resolveVisible();
	update();
}

Grid::Layout Grid::layout(int width) const {
	// YouTube's desktop home: cards at least ~300 px wide, as many columns as fit.
	auto result = Layout();
	result.padding = S(24);
	result.gap = S(16);
	result.rowGap = S(28);
	const auto inner = std::max(width - 2 * result.padding, S(200));
	result.columns = std::max(1, (inner + result.gap) / (S(300) + result.gap));
	result.cardWidth = (inner - (result.columns - 1) * result.gap) / result.columns;
	result.thumbHeight = result.cardWidth * 9 / 16;
	result.rowHeight = result.thumbHeight
		+ S(12)
		+ st::semiboldFont->height * 2
		+ S(4)
		+ st::normalFont->height;
	return result;
}

int Grid::resizeGetHeight(int newWidth) {
	_layout = layout(newWidth);
	const auto rows = (int(_cards.size()) + _layout.columns - 1) / _layout.columns;
	const auto content = rows * _layout.rowHeight
		+ std::max(rows - 1, 0) * _layout.rowGap;
	const auto footer = (_list->exhausted() ? 0 : S(48));
	return std::max(
		2 * _layout.padding + content + footer,
		_cards.empty() ? S(400) : 0);
}

QRect Grid::cardRect(int index) const {
	const auto &l = _layout;
	const auto row = index / l.columns, column = index % l.columns;
	return QRect(
		l.padding + column * (l.cardWidth + l.gap),
		l.padding + row * (l.rowHeight + l.rowGap),
		l.cardWidth,
		l.rowHeight);
}

int Grid::cardAt(QPoint point) const {
	for (auto i = 0; i != int(_cards.size()); ++i) {
		if (cardRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void Grid::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
	resolveVisible();
	const auto rowSpan = _layout.rowHeight + _layout.rowGap;
	if (visibleBottom + kLoadMoreRows * rowSpan >= height()) {
		_list->loadMore();
	}
}

void Grid::resolveVisible() {
	// Read-ahead: the visible cards and one more row, through getWebPage only (never a resolve).
	const auto rowSpan = std::max(_layout.rowHeight + _layout.rowGap, 1);
	if (_visibleBottom <= _visibleTop) {
		// Not scrolled yet: the scroll area has not reported a range — the viewport is it.
		_visibleTop = 0;
		_visibleBottom = parentWidget() ? parentWidget()->height() : rowSpan;
	}
	for (auto i = 0; i != int(_cards.size()); ++i) {
		auto &card = _cards[i];
		const auto item = card.item;
		const auto rect = cardRect(i);
		if (rect.y() > _visibleBottom + rowSpan || rect.y() + rect.height() < _visibleTop) {
			continue;
		}
		if (card.item->document) {
			RefreshCard(card);
			Upgrade(_feed, card, this, [=] { retitle(item); });
			continue;
		}
		if (card.asked) {
			continue;
		}
		card.asked = true;
		_feed->resolve(item, false, crl::guard(this, [=] {
			if (item->failed) {
				_list->remove(item); // a dead post leaves the grid, as on Android
				return;
			}
			resolveVisible();
			update();
		}), true);
	}
}

void Grid::retitle(const std::shared_ptr<Item> &item) {
	const auto i = ranges::find(_cards, item, &Card::item);
	if (i != end(_cards)) {
		i->retitle = true;
	}
	update();
}

void Grid::paintEvent(QPaintEvent *e) {
	if (_cards.size() != _list->items().size()) {
		sync(); // never paint "Loading" over a list that has arrived
	}
	auto p = Painter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::windowBg);
	if (_cards.empty()) {
		const auto status = _list->status();
		if (!status.isEmpty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, status);
		}
		return;
	}
	for (auto i = 0; i != int(_cards.size()); ++i) {
		const auto rect = cardRect(i);
		if (!rect.intersects(clip)) {
			continue;
		}
		auto &card = _cards[i];
		RefreshCard(card);
		const auto thumb = QRect(rect.x(), rect.y(), rect.width(), _layout.thumbHeight);
		PaintThumb(p, card, thumb, S(12));
		if (_over == i) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 255, 255, 18));
			p.drawRoundedRect(thumb, S(12), S(12));
		}
		const auto top = thumb.y() + thumb.height() + S(12);
		const auto avatar = S(36);
		PaintUserpic(p, _session, card, rect.x(), top, avatar);
		const auto left = rect.x() + avatar + S(12);
		const auto available = rect.width() - avatar - S(12);
		p.setPen(st::windowFg);
		if (!card.title.isEmpty()) {
			card.title.drawElided(p, left, top, available, 2);
		}
		const auto lineHeight = st::semiboldFont->height;
		const auto titleHeight = card.title.isEmpty()
			? 0
			: (card.title.maxWidth() > available ? 2 : 1) * lineHeight;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto meta = st::normalFont->elided(MetaLine(_session, *card.item), available);
		p.drawText(
			left,
			top + titleHeight + S(4) + st::normalFont->ascent,
			meta);
	}
}

void Grid::mouseMoveEvent(QMouseEvent *e) {
	const auto over = cardAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor(over >= 0 ? style::cur_pointer : style::cur_default);
		update();
	}
}

void Grid::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		setCursor(style::cur_default);
		update();
	}
}

void Grid::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) ? cardAt(e->pos()) : -1;
}

void Grid::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == cardAt(e->pos()) && pressed < int(_cards.size())) {
		_openRequests.fire_copy(_cards[pressed].item);
	}
}

// ---- The watch page ------------------------------------------------------------------------

class WatchPage final : public Ui::RpWidget {
public:
	WatchPage(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Reels::Feed*> feed,
		std::shared_ptr<Item> item);
	~WatchPage();

	[[nodiscard]] rpl::producer<std::shared_ptr<Item>> openRequests() const {
		return _openRequests.events();
	}
	[[nodiscard]] rpl::producer<bool> fullscreenRequests() const {
		return _fullscreenRequests.events();
	}
	void setViewportHeight(int height);
	void setFullscreen(bool fullscreen);
	[[nodiscard]] bool fullscreen() const {
		return _fullscreen;
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	enum class Hit {
		None,
		Player,
		PlayButton,
		Seek,
		Fullscreen,
		Channel,
		Subscribe,
		Like,
		Share,
		Save,
		More,
		Related,
	};
	struct Layout {
		QRect player;
		QRect controls;
		QRect playButton;
		QRect seek;
		QRect fullscreen;
		QRect title;
		QRect channel;
		QRect subscribe;
		QRect like;
		QRect share;
		QRect save;
		QRect caption;
		QRect more;
		QRect relatedHeader;
		int relatedX = 0;
		int relatedY = 0;
		int relatedWidth = 0;
		int relatedRow = 0;
	};

	[[nodiscard]] Layout computeLayout(int width) const;
	[[nodiscard]] Hit hitAt(QPoint point, int *related = nullptr) const;
	void start();
	void play(crl::time position);
	void togglePause();
	void seekTo(float64 fraction);
	void check();
	void openFullscreen();
	void withMessage(Fn<void(not_null<HistoryItem*>)> callback);
	void refreshTexts();
	void syncRelated();
	void paintPlayer(Painter &p);

	const not_null<Window::SessionController*> _controller;
	const not_null<Main::Session*> _session;
	const not_null<Reels::Feed*> _feed;
	const std::shared_ptr<Item> _item;
	std::unique_ptr<List> _related;
	std::vector<Card> _relatedCards;
	Card _self;

	std::unique_ptr<::Media::Streaming::Instance> _instance;
	std::unique_ptr<Watch> _watch;
	bool _withSound = false;
	bool _userPaused = false;
	bool _ended = false;
	bool _firstFrame = false;
	base::Timer _checkTimer;

	Ui::Text::String _title;
	Ui::Text::String _caption;
	bool _captionExpanded = false;
	Layout _layout;
	int _viewportHeight = 0;
	Hit _over = Hit::None;
	int _overRelated = -1;
	Hit _pressed = Hit::None;
	int _pressedRelated = -1;
	bool _controlsShown = true;
	rpl::event_stream<std::shared_ptr<Item>> _openRequests;
	rpl::event_stream<bool> _fullscreenRequests;
	bool _fullscreen = false;
	bool _socialWatched = false;

};

WatchPage::WatchPage(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Reels::Feed*> feed,
	std::shared_ptr<Item> item)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _feed(feed)
, _item(std::move(item))
, _self{ .item = _item }
, _checkTimer([=] { check(); }) {
	setMouseTracking(true);
	_related = std::make_unique<List>(
		feed,
		u"/v1/videos/related?seed_channel_id=%1&seed_message_id=%2"_q
			.arg(_item->channelId)
			.arg(_item->messageId.bare),
		kRelatedPageSize);
	_related->updates() | rpl::on_next([=] {
		syncRelated();
	}, lifetime());
	_related->loadMore();
	_watch = std::make_unique<Watch>(feed, _item);
	_checkTimer.callEach(kCheckPeriod);
	refreshTexts();
	start();
}

WatchPage::~WatchPage() {
	if (_instance) {
		check(); // the last position and watch time, before the leave event is classified
		_instance->stop();
	}
	_watch = nullptr;
}

void WatchPage::setViewportHeight(int height) {
	if (_viewportHeight != height) {
		_viewportHeight = height;
		resizeToWidth(width());
	}
}

void WatchPage::setFullscreen(bool fullscreen) {
	if (_fullscreen != fullscreen) {
		_fullscreen = fullscreen;
		resizeToWidth(width());
		update();
	}
}

void WatchPage::refreshTexts() {
	const auto text = _item->caption.text.trimmed();
	_title.setText(st::boxTitle.style, text.section('\n', 0, 0));
	auto rest = _item->caption;
	const auto newline = rest.text.indexOf('\n');
	if (newline >= 0) {
		rest = Ui::Text::Mid(rest, newline + 1);
	} else {
		rest = TextWithEntities();
	}
	_caption.setMarkedText(st::defaultTextStyle, rest);
}

void WatchPage::start() {
	if (!_socialWatched) {
		_socialWatched = true;
		_feed->loadSocial(_item);
		_feed->updates() | rpl::on_next([=] {
			update();
		}, lifetime());
	}
	if (!_item->document) {
		const auto weak = base::make_weak(this);
		_feed->resolve(_item, true, [=] {
			if (!weak) {
				return;
			} else if (_item->document) {
				refreshTexts();
				resizeToWidth(width());
				start();
			}
			update();
		});
		return;
	}
	if (_instance) {
		return;
	}
	RefreshCard(_self);
	const auto document = _item->document;
	_withSound = !document->isSilentVideo();
	_instance = std::make_unique<::Media::Streaming::Instance>(
		document,
		_item->origin,
		[=] { update(_layout.player); });
	const auto instance = _instance.get();
	instance->lockPlayer();
	instance->setPriority(kLoaderPriority);
	instance->player().updates(
	) | rpl::on_next_error([=](::Media::Streaming::Update &&update) {
		if (v::is<::Media::Streaming::Finished>(update.data)) {
			_ended = true;
			_watch->ended();
		}
		this->update(_layout.player);
	}, [=](::Media::Streaming::Error &&error) {
		update(_layout.player);
	}, instance->lifetime());
	play(0);
}

void WatchPage::play(crl::time position) {
	if (!_instance) {
		return;
	}
	auto options = ::Media::Streaming::PlaybackOptions{
		.position = position,
		.hwAllowed = Core::App().settings().hardwareAcceleratedVideo(),
	};
	if (_withSound) {
		options.audioId = AudioMsgId(_item->document, _item->fullId);
		Media::Player::instance()->pause(AudioMsgId::Type::Voice);
		Media::Player::instance()->pause(AudioMsgId::Type::Song);
		Media::Player::mixer()->setVideoVolume(Core::App().settings().videoVolume());
	} else {
		options.mode = ::Media::Streaming::Mode::Video;
	}
	_ended = false;
	_instance->play(options);
	if (_userPaused) {
		_instance->pause();
	}
}

void WatchPage::togglePause() {
	if (!_instance) {
		return;
	}
	if (_ended) {
		_userPaused = false;
		play(0);
		return;
	}
	_userPaused = !_userPaused;
	check();
	update(_layout.player);
}

void WatchPage::seekTo(float64 fraction) {
	if (!_instance) {
		return;
	}
	const auto &track = _instance->info().video.state;
	const auto duration = (track.duration > 0)
		? track.duration
		: (_item->document ? _item->document->duration() : 0);
	if (duration > 0) {
		play(crl::time(std::clamp(fraction, 0., 1.) * duration));
	}
}

void WatchPage::check() {
	if (!_instance) {
		return;
	}
	const auto instance = _instance.get();
	const auto allowed = isVisible()
		&& !_userPaused
		&& !_ended
		&& !_controller->isLayerShown();
	if (allowed && instance->paused()) {
		instance->resume();
	} else if (!allowed && instance->active() && !instance->paused()) {
		instance->pause();
	}
	const auto ready = instance->player().ready();
	if (ready && !_firstFrame) {
		_firstFrame = true;
		_watch->firstFrame();
	}
	const auto waiting = instance->waitingShown();
	const auto &track = instance->info().video.state;
	_watch->buffering(allowed && waiting);
	_watch->playing(
		allowed && ready && !waiting,
		(track.position != ::Media::kTimeUnknown) ? track.position : -1,
		track.duration);
	if (ready && allowed) {
		update(_layout.controls);
	}
}

void WatchPage::openFullscreen() {
	// Our own player, the whole screen: it needs no Telegram message, it keeps playing where it is.
	_fullscreenRequests.fire(!_fullscreen);
}

void WatchPage::withMessage(Fn<void(not_null<HistoryItem*>)> callback) {
	const auto weak = base::make_weak(this);
	_feed->requireMessage(_item, [=](HistoryItem *message) {
		if (!weak) {
			return;
		} else if (!message) {
			_controller->showToast(Tr(Str::ReelsActionUnavailable));
			return;
		}
		callback(message);
		update();
	});
}

void WatchPage::syncRelated() {
	const auto &items = _related->items();
	auto cards = std::vector<Card>();
	for (const auto &item : items) {
		if (item->channelId == _item->channelId && item->messageId == _item->messageId) {
			continue; // the seed comes back in its own related list; Android drops it too
		}
		const auto i = ranges::find(_relatedCards, item, &Card::item);
		cards.push_back((i != end(_relatedCards)) ? std::move(*i) : Card{ .item = item });
	}
	_relatedCards = std::move(cards);
	resizeToWidth(width());
	update();
}

WatchPage::Layout WatchPage::computeLayout(int width) const {
	auto l = Layout();
	if (_fullscreen) {
		l.player = QRect(0, 0, width, std::max(_viewportHeight, S(200)));
		const auto bar = S(48);
		l.controls = QRect(0, l.player.height() - bar, width, bar);
		l.playButton = QRect(S(12), l.controls.y() + S(8), S(32), S(32));
		l.fullscreen = QRect(width - S(48), l.controls.y() + S(8), S(32), S(32));
		l.seek = QRect(0, l.controls.y() - S(6), width, S(12));
		return l;
	}
	const auto padding = S(24);
	const auto wide = (width >= S(1000));
	l.relatedWidth = wide ? S(400) : (width - 2 * padding);
	const auto mainWidth = wide
		? (width - 2 * padding - S(24) - l.relatedWidth)
		: (width - 2 * padding);
	auto playerHeight = mainWidth * 9 / 16;
	if (_viewportHeight > 0) {
		playerHeight = std::min(playerHeight, _viewportHeight * 7 / 10);
	}
	l.player = QRect(padding, padding, mainWidth, playerHeight);
	const auto bar = S(40);
	l.controls = QRect(l.player.x(), l.player.y() + l.player.height() - bar, l.player.width(), bar);
	l.playButton = QRect(l.controls.x() + S(8), l.controls.y() + S(4), S(32), S(32));
	l.fullscreen = QRect(l.controls.x() + l.controls.width() - S(40), l.controls.y() + S(4), S(32), S(32));
	l.seek = QRect(l.controls.x(), l.controls.y() - S(6), l.controls.width(), S(12));

	auto y = l.player.y() + l.player.height() + S(16);
	const auto titleLine = std::max(
		st::boxTitle.style.lineHeight,
		st::boxTitle.style.font->height);
	const auto titleHeight = _title.isEmpty()
		? 0
		: std::min(_title.countHeight(mainWidth), titleLine * 2) + S(4);
	l.title = QRect(padding, y, mainWidth, titleHeight);
	y += titleHeight + S(12);
	l.channel = QRect(padding, y, mainWidth / 2, S(40));
	const auto pillHeight = S(36);
	const auto subscribeWidth = std::max(
		st::semiboldFont->width(Tr(Str::ReelsSubscribe)),
		st::semiboldFont->width(Tr(Str::ReelsSubscribed))) + S(32);
	l.subscribe = QRect(
		padding + S(40) + S(12) + std::min(S(220), mainWidth / 3),
		y + (S(40) - pillHeight) / 2,
		subscribeWidth,
		pillHeight);
	l.channel.setWidth(l.subscribe.x() - padding);
	auto x = padding + mainWidth;
	const auto pill = [&](const QString &text) {
		const auto w = st::semiboldFont->width(text) + S(32) + S(20);
		x -= w;
		const auto r = QRect(x, y + (S(40) - pillHeight) / 2, w, pillHeight);
		x -= S(8);
		return r;
	};
	l.save = pill(Tr(Str::ReelsSave));
	l.share = pill(Tr(Str::ReelsShare));
	l.like = pill(u"999K"_q);
	y += S(40) + S(16);
	if (!_caption.isEmpty()) {
		const auto full = _caption.countHeight(mainWidth - 2 * S(12));
		const auto collapsed = st::normalFont->height * 4;
		const auto shown = _captionExpanded ? full : std::min(full, collapsed);
		l.caption = QRect(padding, y, mainWidth, shown + 2 * S(12));
		if (full > collapsed) {
			l.more = QRect(padding + S(12), l.caption.y() + l.caption.height(), mainWidth / 2, st::normalFont->height + S(8));
			y = l.more.y() + l.more.height();
		} else {
			y = l.caption.y() + l.caption.height();
		}
		y += S(16);
	}
	l.relatedRow = S(94) + S(12);
	if (wide) {
		l.relatedX = padding + mainWidth + S(24);
		l.relatedHeader = QRect(l.relatedX, padding, l.relatedWidth, st::semiboldFont->height + S(12));
	} else {
		l.relatedX = padding;
		l.relatedHeader = QRect(padding, y + S(8), l.relatedWidth, st::semiboldFont->height + S(12));
	}
	l.relatedY = l.relatedHeader.y() + l.relatedHeader.height();
	return l;
}

int WatchPage::resizeGetHeight(int newWidth) {
	_layout = computeLayout(newWidth);
	if (_fullscreen) {
		return _layout.player.height();
	}
	const auto relatedBottom = _layout.relatedY
		+ int(_relatedCards.size()) * _layout.relatedRow;
	const auto mainBottom = std::max({
		_layout.player.y() + _layout.player.height(),
		_layout.caption.y() + _layout.caption.height(),
		_layout.more.y() + _layout.more.height(),
	});
	return std::max(relatedBottom, mainBottom) + S(48);
}

void WatchPage::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	// Resolve the related rows on screen (read-ahead, no resolve spent) and page on.
	for (auto i = 0; i != int(_relatedCards.size()); ++i) {
		const auto y = _layout.relatedY + i * _layout.relatedRow;
		if (y > visibleBottom || y + _layout.relatedRow < visibleTop) {
			continue;
		}
		auto &card = _relatedCards[i];
		if (!card.asked && !card.item->document) {
			card.asked = true;
			_feed->resolve(card.item, false, crl::guard(this, [=] { update(); }), true);
		}
	}
	if (visibleBottom + 2 * _layout.relatedRow >= height()) {
		_related->loadMore();
	}
}

void WatchPage::paintPlayer(Painter &p) {
	const auto &r = _layout.player;
	auto clip = QPainterPath();
	const auto radius = _fullscreen ? 0 : S(12);
	clip.addRoundedRect(r, radius, radius);
	p.save();
	p.setClipPath(clip);
	p.fillRect(r, QColor(0, 0, 0));
	const auto instance = _instance.get();
	const auto ready = instance
		&& instance->player().ready()
		&& !instance->player().videoSize().isEmpty();
	if (ready) {
		const auto size = instance->player().videoSize().scaled(r.size(), Qt::KeepAspectRatio);
		const auto target = QRect(
			r.x() + (r.width() - size.width()) / 2,
			r.y() + (r.height() - size.height()) / 2,
			size.width(),
			size.height());
		const auto ratio = style::DevicePixelRatio();
		p.drawImage(target, instance->frame({
			.resize = target.size() * ratio,
			.outer = target.size() * ratio,
		}));
		instance->markFrameShown();
	} else {
		PaintThumb(p, _self, r, 0, false); // the player has its own time line
	}

	const auto showControls = _controlsShown || _userPaused || _ended || !ready;
	if (showControls) {
		auto gradient = QLinearGradient(0, _layout.controls.y() - S(40), 0, r.y() + r.height());
		gradient.setColorAt(0., QColor(0, 0, 0, 0));
		gradient.setColorAt(1., QColor(0, 0, 0, 160));
		p.fillRect(QRect(r.x(), _layout.controls.y() - S(40), r.width(), S(40) + _layout.controls.height()), gradient);

		auto hq = PainterHighQualityEnabler(p);
		// Play / pause.
		const auto b = _layout.playButton.marginsRemoved({ S(8), S(8), S(8), S(8) });
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255));
		if (_userPaused || _ended || !instance) {
			auto path = QPainterPath();
			path.moveTo(b.x() + b.width() * 0.15, b.y());
			path.lineTo(b.x() + b.width() * 0.15, b.y() + b.height());
			path.lineTo(b.x() + b.width(), b.y() + b.height() / 2.);
			path.closeSubpath();
			p.drawPath(path);
		} else {
			const auto w = b.width() / 3;
			p.drawRect(QRect(b.x() + w / 4, b.y(), w, b.height()));
			p.drawRect(QRect(b.x() + b.width() - w - w / 4, b.y(), w, b.height()));
		}
		// Fullscreen: four corners.
		const auto f = _layout.fullscreen.marginsRemoved({ S(8), S(8), S(8), S(8) });
		auto pen = QPen(QColor(255, 255, 255), S(2));
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto c = S(5);
		p.drawPolyline(QPolygon({ QPoint(f.left(), f.top() + c), f.topLeft(), QPoint(f.left() + c, f.top()) }));
		p.drawPolyline(QPolygon({ QPoint(f.right() - c, f.top()), f.topRight(), QPoint(f.right(), f.top() + c) }));
		p.drawPolyline(QPolygon({ QPoint(f.left(), f.bottom() - c), f.bottomLeft(), QPoint(f.left() + c, f.bottom()) }));
		p.drawPolyline(QPolygon({ QPoint(f.right() - c, f.bottom()), f.bottomRight(), QPoint(f.right(), f.bottom() - c) }));

		// Time.
		if (instance) {
			const auto &track = instance->info().video.state;
			const auto duration = (track.duration > 0)
				? track.duration
				: (_item->document ? _item->document->duration() : 0);
			const auto position = (track.position != ::Media::kTimeUnknown) ? track.position : 0;
			const auto text = DurationText(std::max(position, crl::time(1000)))
				+ u" / "_q + DurationText(duration);
			p.setFont(st::normalFont);
			p.setPen(QColor(255, 255, 255));
			p.drawText(
				_layout.playButton.x() + _layout.playButton.width() + S(8),
				_layout.controls.y() + (_layout.controls.height() + st::normalFont->ascent - st::normalFont->descent) / 2,
				text);
			// Seek line.
			const auto line = QRect(r.x(), _layout.controls.y() - S(2), r.width(), S(3));
			p.fillRect(line, QColor(255, 255, 255, 70));
			if (duration > 0) {
				const auto played = int(line.width() * std::clamp(position / float64(duration), 0., 1.));
				p.fillRect(QRect(line.x(), line.y(), played, line.height()), QColor(0xFF, 0x2E, 0x38));
			}
		}
	}
	if (instance && instance->waitingShown() && !_userPaused) {
		p.setFont(st::normalFont);
		p.setPen(QColor(255, 255, 255, 200));
		p.drawText(r, Qt::AlignCenter, Tr(Str::ReelsLoadingFeed));
	}
	p.restore();
}

void WatchPage::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	p.fillRect(e->rect(), _fullscreen ? QColor(0, 0, 0) : st::windowBg->c);
	paintPlayer(p);
	if (_fullscreen) {
		return;
	}
	const auto &l = _layout;

	if (!_title.isEmpty()) {
		p.setPen(st::windowFg);
		_title.drawElided(p, l.title.x(), l.title.y(), l.title.width(), 2);
	}
	// Channel row.
	PaintUserpic(p, _session, _self, l.channel.x(), l.channel.y(), S(40));
	const auto channel = ChannelOf(_session, *_item);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	const auto nameLeft = l.channel.x() + S(52);
	const auto name = channel ? channel->name() : (u"@"_q + _item->username);
	p.drawText(
		nameLeft,
		l.channel.y() + (S(40) + st::semiboldFont->ascent - st::semiboldFont->descent) / 2,
		st::semiboldFont->elided(name, l.channel.width() - S(52)));

	auto hq = PainterHighQualityEnabler(p);
	const auto pill = [&](QRect r, const QString &text, bool accent, bool hovered) {
		p.setPen(Qt::NoPen);
		p.setBrush(accent
			? (hovered ? st::activeButtonBgOver : st::activeButtonBg)
			: (hovered ? st::windowBgOver : st::windowBgRipple));
		p.drawRoundedRect(r, r.height() / 2., r.height() / 2.);
		p.setFont(st::semiboldFont);
		p.setPen(accent ? st::activeButtonFg : st::windowFg);
		p.drawText(r, Qt::AlignCenter, text);
	};
	// Svipe's own subscribe and like (our server), never Telegram's join or reaction.
	const auto following = _feed->following(_item->channelId);
	pill(
		l.subscribe,
		following ? Tr(Str::ReelsSubscribed) : Tr(Str::ReelsSubscribe),
		!following,
		_over == Hit::Subscribe);
	const auto likes = _item->likes;
	const auto liked = _item->liked;
	const auto likeText = QString::fromUtf8("\xe2\x9d\xa4 ")
		+ (likes > 0 ? Lang::FormatCountToShort(likes).string : QString());
	pill(l.like, likeText.trimmed(), liked, _over == Hit::Like);
	pill(l.share, Tr(Str::ReelsShare), false, _over == Hit::Share);
	pill(l.save, Tr(Str::ReelsSave), false, _over == Hit::Save);

	// Caption box.
	if (!l.caption.isEmpty()) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(l.caption, S(12), S(12));
		p.setPen(st::windowFg);
		const auto lines = _captionExpanded ? -1 : 4;
		if (lines < 0) {
			_caption.draw(p, {
				.position = { l.caption.x() + S(12), l.caption.y() + S(12) },
				.availableWidth = l.caption.width() - 2 * S(12),
			});
		} else {
			_caption.drawElided(p, l.caption.x() + S(12), l.caption.y() + S(12), l.caption.width() - 2 * S(12), lines);
		}
		if (!l.more.isEmpty()) {
			p.setFont(st::semiboldFont);
			p.setPen(st::windowActiveTextFg);
			p.drawText(l.more, Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(_captionExpanded
				? "\xe2\x96\xb2"
				: "\xe2\x96\xbc \xe2\x80\xa6"));
		}
	}

	// Related.
	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	p.drawText(l.relatedHeader, Qt::AlignLeft | Qt::AlignVCenter, Tr(Str::RelatedVideos));
	for (auto i = 0; i != int(_relatedCards.size()); ++i) {
		const auto y = l.relatedY + i * l.relatedRow;
		const auto row = QRect(l.relatedX, y, l.relatedWidth, l.relatedRow - S(12));
		if (!row.intersects(e->rect())) {
			continue;
		}
		auto &card = _relatedCards[i];
		RefreshCard(card);
		const auto thumb = QRect(row.x(), row.y(), S(168), S(94));
		PaintThumb(p, card, thumb, S(8));
		if (_overRelated == i) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 255, 255, 18));
			p.drawRoundedRect(thumb, S(8), S(8));
		}
		const auto left = thumb.x() + thumb.width() + S(10);
		const auto available = row.width() - (left - row.x());
		p.setPen(st::windowFg);
		if (!card.title.isEmpty()) {
			card.title.drawElided(p, left, row.y(), available, 2);
		}
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			left,
			row.y() + 2 * st::semiboldFont->height + S(4) + st::normalFont->ascent,
			st::normalFont->elided(MetaLine(_session, *card.item), available));
	}
}

WatchPage::Hit WatchPage::hitAt(QPoint point, int *related) const {
	const auto &l = _layout;
	if (l.player.contains(point)) {
		if (l.seek.contains(point)) return Hit::Seek;
		if (l.playButton.contains(point)) return Hit::PlayButton;
		if (l.fullscreen.contains(point)) return Hit::Fullscreen;
		return Hit::Player;
	}
	if (_fullscreen) {
		return Hit::None;
	}
	const auto channel = ChannelOf(_session, *_item);
	if (l.subscribe.contains(point)) return Hit::Subscribe;
	if (l.channel.contains(point)) return Hit::Channel;
	if (l.like.contains(point)) return Hit::Like;
	if (l.share.contains(point)) return Hit::Share;
	if (l.save.contains(point)) return Hit::Save;
	if (l.more.contains(point)) return Hit::More;
	for (auto i = 0; i != int(_relatedCards.size()); ++i) {
		const auto row = QRect(l.relatedX, l.relatedY + i * l.relatedRow, l.relatedWidth, l.relatedRow - S(12));
		if (row.contains(point)) {
			if (related) {
				*related = i;
			}
			return Hit::Related;
		}
	}
	return Hit::None;
}

void WatchPage::mouseMoveEvent(QMouseEvent *e) {
	auto related = -1;
	const auto over = hitAt(e->pos(), &related);
	const auto controls = _layout.player.contains(e->pos());
	if (_over != over || _overRelated != related || _controlsShown != controls) {
		_over = over;
		_overRelated = related;
		_controlsShown = controls;
		setCursor((over == Hit::None || over == Hit::Player) ? style::cur_default : style::cur_pointer);
		update();
	}
}

void WatchPage::leaveEventHook(QEvent *e) {
	_over = Hit::None;
	_overRelated = -1;
	_controlsShown = false;
	setCursor(style::cur_default);
	update();
}

void WatchPage::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = hitAt(e->pos(), &_pressedRelated);
	if (_pressed == Hit::Seek) {
		const auto &r = _layout.player;
		seekTo((e->pos().x() - r.x()) / float64(std::max(r.width(), 1)));
	}
}

void WatchPage::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && hitAt(e->pos()) == Hit::Player) {
		togglePause(); // undo the first click's pause: a double click means fullscreen, as on YouTube
		openFullscreen();
		return;
	}
	mousePressEvent(e);
}

void WatchPage::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	auto related = -1;
	const auto hit = hitAt(e->pos(), &related);
	const auto pressed = std::exchange(_pressed, Hit::None);
	if (hit != pressed) {
		return;
	}
	switch (hit) {
	case Hit::Player:
	case Hit::PlayButton: togglePause(); break;
	case Hit::Fullscreen: openFullscreen(); break;
	case Hit::Channel:
		withMessage([=](not_null<HistoryItem*> message) {
			const auto peer = message->history()->peer;
			const auto id = message->id;
			const auto controller = _controller;
			Close(controller);
			controller->showPeerHistory(peer, Window::SectionShow::Way::ClearStack, id);
		});
		break;
	case Hit::Subscribe:
		_feed->toggleFollow(_item);
		break;
	case Hit::Like:
		_feed->toggleLike(_item);
		break;
	case Hit::Share: {
		QGuiApplication::clipboard()->setText(!_item->shareUrl.isEmpty()
			? _item->shareUrl
			: u"https://t.me/%1/%2"_q.arg(_item->username).arg(_item->messageId.bare));
		_controller->showToast(tr::lng_channel_public_link_copied(tr::now));
		_feed->sendEvent(*_item, u"SHARE"_q);
	} break;
	case Hit::Save:
		_feed->saveDocument(_item, crl::guard(this, [=](ChannelData *channel) {
			_controller->showToast(channel
				? (Tr(Str::ReelsSavedChannel) + u" ✓"_q)
				: Tr(Str::ReelsActionUnavailable));
		}));
		break;
	case Hit::More:
		_captionExpanded = !_captionExpanded;
		resizeToWidth(width());
		update();
		break;
	case Hit::Related:
		if (related >= 0 && related < int(_relatedCards.size())) {
			_openRequests.fire_copy(_relatedCards[related].item);
		}
		break;
	default: break;
	}
}

// ---- The overlay ---------------------------------------------------------------------------

class Widget final : public Ui::RpWidget {
public:
	Widget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Reels::Feed*> feed,
		not_null<List*> list);

	void openVideo(std::shared_ptr<Item> item);
	void back();
	void showGrid();
	void placeIn(QSize body);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	const not_null<Window::SessionController*> _controller;
	const not_null<Reels::Feed*> _feed;
	object_ptr<Ui::ScrollArea> _browse;
	object_ptr<Ui::ScrollArea> _watch = { nullptr };
	object_ptr<Ui::IconButton> _back = { nullptr };
	QPointer<WatchPage> _page;
	bool _fullscreen = false;

	void layoutWatch();
	bool _windowWasFullScreen = false;

	void setFullscreen(bool fullscreen);

};

Widget::Widget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Reels::Feed*> feed,
	not_null<List*> list)
: RpWidget(parent)
, _controller(controller)
, _feed(feed)
, _browse(this, st::defaultScrollArea) {
	setFocusPolicy(Qt::StrongFocus);
	const auto grid = _browse->setOwnedWidget(object_ptr<Grid>(
		this,
		&controller->session(),
		feed,
		list));
	grid->openRequests() | rpl::on_next([=](std::shared_ptr<Item> item) {
		openVideo(item);
	}, grid->lifetime());
}

void Widget::openVideo(std::shared_ptr<Item> item) {
	setFullscreen(false);
	_page = nullptr;
	_watch.destroy();
	_watch.create(this, st::defaultScrollArea);
	const auto page = _watch->setOwnedWidget(object_ptr<WatchPage>(
		this,
		_controller,
		_feed,
		item));
	_page = page.data();
	page->fullscreenRequests() | rpl::on_next([=](bool fullscreen) {
		setFullscreen(fullscreen);
	}, page->lifetime());
	page->openRequests() | rpl::on_next([=](std::shared_ptr<Item> next) {
		// A related video replaces this page; back still returns to the grid.
		crl::on_main(this, [=] { openVideo(next); });
	}, page->lifetime());
	if (!_back) {
		_back.create(this, st::infoTopBarBack);
		_back->setClickedCallback([=] { showGrid(); });
	}
	layoutWatch();
	_watch->show();
	_back->show();
	_back->raise();
	_browse->hide();
	setFocus();
}

void Widget::layoutWatch() {
	if (!_watch) {
		return;
	}
	// A back bar above the page, as YouTube's header: the way back to the grid without a keyboard.
	const auto bar = _fullscreen ? 0 : st::infoTopBarHeight;
	if (_back) {
		_back->setVisible(!_fullscreen);
		_back->moveToLeft(0, 0);
	}
	_watch->setGeometry(0, bar, width(), height() - bar);
	if (_page) {
		_page->setViewportHeight(height() - bar);
		_page->resizeToWidth(width());
	}
}

void Widget::showGrid() {
	if (_fullscreen) {
		setFullscreen(false);
	}
	if (_watch) {
		_page = nullptr;
		_watch.destroy();
		_back.destroy();
		_browse->show();
		update();
	}
	setFocus();
}

void Widget::placeIn(QSize body) {
	const auto left = (_fullscreen || !_controller->hasFiltersMenu())
		? 0
		: st::windowFiltersWidth;
	setGeometry(QRect(left, 0, body.width() - left, body.height()));
}

void Widget::setFullscreen(bool fullscreen) {
	if (_fullscreen == fullscreen) {
		return;
	}
	_fullscreen = fullscreen;
	const auto window = _controller->widget();
	if (fullscreen) {
		_windowWasFullScreen = window->isFullScreen();
		if (!_windowWasFullScreen) {
			window->showFullScreen();
		}
	} else if (!_windowWasFullScreen) {
		window->showNormal();
	}
	if (_watch) {
		_watch->scrollToY(0);
	}
	if (_page) {
		_page->setFullscreen(fullscreen);
	}
	layoutWatch();
	if (parentWidget()) {
		placeIn(parentWidget()->size());
	}
	raise();
	setFocus();
}

void Widget::back() {
	if (_fullscreen) {
		setFullscreen(false);
		return;
	}
	if (_watch) {
		showGrid();
	} else {
		Close(_controller);
	}
}

void Widget::resizeEvent(QResizeEvent *e) {
	_browse->setGeometry(rect());
	if (const auto grid = static_cast<Grid*>(_browse->widget())) {
		grid->resizeToWidth(width());
	}
	layoutWatch();
}

void Widget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
}

void Widget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) {
		back();
		return;
	}
	RpWidget::keyPressEvent(e);
}

struct Holder {
	std::unique_ptr<Reels::Feed> feed;
	std::unique_ptr<List> list;
	crl::time listAt = 0;
	base::unique_qptr<Widget> view;
	rpl::variable<bool> shown = false;
};

base::flat_map<not_null<Window::SessionController*>, Holder> &Holders() {
	static auto result = base::flat_map<
		not_null<Window::SessionController*>,
		Holder>();
	return result;
}

Holder &HolderFor(not_null<Window::SessionController*> controller) {
	auto &holders = Holders();
	auto i = holders.find(controller);
	if (i == end(holders)) {
		i = holders.emplace(controller, Holder()).first;
		controller->lifetime().add([=] {
			Holders().remove(controller);
		});
	}
	return i->second;
}

} // namespace

// Android: SvipeVideoWarmer.FRESH_FOR_MS — the pipe is personalised and moves on.
constexpr auto kFreshFor = crl::time(10 * 60 * 1000);
constexpr auto kWarmResolve = 6; // the cards on the first screen
const auto kUsedKey = u"svipe_video_used"_q;

void EnsureList(not_null<Window::SessionController*> controller, Holder &holder) {
	LOG(("Svipe Video: ensure list for controller %1, has list %2, view %3"
		).arg(quintptr(controller.get())
		).arg(Logs::b(holder.list != nullptr)
		).arg(Logs::b(holder.view != nullptr)));
	if (!holder.feed) {
		holder.feed = std::make_unique<Reels::Feed>(&controller->session());
	}
	auto freshFor = kFreshFor;
#ifdef _DEBUG
	if (QFile::exists(cWorkingDir() + u"tdata/svipe_fresh_short"_q)) {
		freshFor = crl::time(10000);
	}
#endif // _DEBUG
	const auto stale = holder.list
		&& !holder.view
		&& (crl::now() - holder.listAt > freshFor);
	if (!holder.list) {
		holder.list = std::make_unique<List>(holder.feed.get(), u"/v1/videos"_q, kPageSize);
		holder.listAt = crl::now();
	} else if (stale) {
		holder.list->reset(); // in place: nothing that holds the list can be left on a dead one
		holder.listAt = crl::now();
	}
}

void Warm(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	if (!Storage::Get(session, kUsedKey).toBool()) {
		return; // a tab you never open should cost you nothing
	}
	auto &holder = HolderFor(controller);
	EnsureList(controller, holder);
	const auto feed = holder.feed.get();
	const auto list = holder.list.get();
	const auto done = std::make_shared<bool>(false);
	const auto lifetime = std::make_shared<rpl::lifetime>();
	list->updates() | rpl::on_next([=] {
		if (*done || list->items().empty()) {
			return;
		}
		*done = true;
		const auto &items = list->items();
		for (auto i = 0; i != std::min(int(items.size()), kWarmResolve); ++i) {
			feed->resolve(items[i], false, nullptr, true);
		}
		lifetime->destroy();
	}, *lifetime);
	list->loadMore();
}

void Open(not_null<Window::SessionController*> controller) {
	controller->hideLayer(anim::type::instant);
	Reels::Close(controller); // one full-window surface at a time
	Storage::Set(&controller->session(), kUsedKey, true);
	auto &holder = HolderFor(controller);
	EnsureList(controller, holder);
	if (holder.view) {
		holder.view->showGrid(); // the Video button again is YouTube's logo: home
		holder.view->raise();
		return;
	}
	const auto body = controller->widget()->bodyWidget();
	holder.view = base::make_unique_q<Widget>(
		body,
		controller,
		holder.feed.get(),
		holder.list.get());
	const auto view = holder.view.get();
	rpl::combine(
		body->sizeValue(),
		rpl::single(rpl::empty) | rpl::then(controller->filtersMenuChanged())
	) | rpl::on_next([=](QSize size, auto) {
		view->placeIn(size);
	}, view->lifetime());
	view->show();
	view->raise();
	view->setFocus();
	holder.shown = true;
}

void Close(not_null<Window::SessionController*> controller) {
	const auto i = Holders().find(controller);
	if (i == end(Holders()) || !i->second.view) {
		return;
	}
	auto view = base::take(i->second.view);
	view->hide();
	view = nullptr;
	i->second.shown = false;
	Ui::ForceFullRepaint(controller->widget());
}

bool Shown(not_null<Window::SessionController*> controller) {
	return HolderFor(controller).shown.current();
}

void DebugOpenFirst(not_null<Window::SessionController*> controller) {
	auto &holder = HolderFor(controller);
	if (!holder.list || !holder.view) {
		return;
	}
	const auto view = holder.view.get();
	const auto list = holder.list.get();
	const auto opened = view->lifetime().make_state<bool>(false);
	const auto tryOpen = [=] {
		if (*opened) {
			return;
		}
		for (const auto &item : list->items()) {
			if (item->document) { // the first one the grid has already resolved
				*opened = true;
				view->openVideo(item);
				return;
			}
		}
	};
	base::timer_each(1000) | rpl::on_next(tryOpen, view->lifetime());
	list->updates() | rpl::on_next(tryOpen, view->lifetime());
	tryOpen();
}

rpl::producer<bool> ShownValue(not_null<Window::SessionController*> controller) {
	return HolderFor(controller).shown.value();
}

} // namespace Svipe::Video
