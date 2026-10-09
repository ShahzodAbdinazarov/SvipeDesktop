/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_reels_section.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "boxes/report_messages_box.h"
#include "core/application.h"
#include "dialogs/dialogs_row.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "core/core_settings.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_preload.h"
#include "data/data_message_reaction_id.h"
#include "data/data_session.h"
#include "data/data_thread.h"
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
#include "svipe/svipe_storage.h"
#include "svipe/svipe_strings.h"
#include "svipe/svipe_video_section.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "window/main_window.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>

namespace Svipe::Reels {
namespace {

// Android: LOAD_MORE_AHEAD, and how far ahead the next clips are looked up.
constexpr auto kLoadMoreAhead = 4;
constexpr auto kResolveAhead = 2;
// Android: SvipeQueuePlan.MIN_WATCHED_MS.
constexpr auto kMinWatched = crl::time(3000);

constexpr auto kSlideDuration = crl::time(220);
constexpr auto kWheelBlock = crl::time(450);
constexpr auto kWheelIdleReset = crl::time(250);
constexpr auto kCheckPeriod = crl::time(200);
constexpr auto kLoaderPriority = 3;

// Android: ReelsActivity.LIKE_EMOJI and the liked heart colour.
const auto kLikeEmoji = QString::fromUtf8("\xe2\x9d\xa4");
const auto kLikedColor = QColor(0xFF, 0x2E, 0x38);
const auto kLinkColor = QColor(0x8A, 0xB4, 0xF8);

[[nodiscard]] int S(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QRect FitInto(QSize size, QRect outer) {
	if (size.isEmpty()) {
		return outer;
	}
	const auto scaled = size.scaled(outer.size(), Qt::KeepAspectRatio);
	return QRect(
		outer.x() + (outer.width() - scaled.width()) / 2,
		outer.y() + (outer.height() - scaled.height()) / 2,
		scaled.width(),
		scaled.height());
}

[[nodiscard]] QPainterPath HeartPath(QRectF r) {
	auto path = QPainterPath();
	const auto w = r.width();
	const auto h = r.height();
	const auto x = r.x();
	const auto y = r.y();
	path.moveTo(x + w / 2, y + h * 0.92);
	path.cubicTo(x + w * 0.1, y + h * 0.62, x - w * 0.02, y + h * 0.3, x + w * 0.22, y + h * 0.12);
	path.cubicTo(x + w * 0.38, y, x + w * 0.5, y + h * 0.12, x + w / 2, y + h * 0.24);
	path.cubicTo(x + w * 0.5, y + h * 0.12, x + w * 0.62, y, x + w * 0.78, y + h * 0.12);
	path.cubicTo(x + w * 1.02, y + h * 0.3, x + w * 0.9, y + h * 0.62, x + w / 2, y + h * 0.92);
	path.closeSubpath();
	return path;
}

[[nodiscard]] QString ShareLink(const Item &item) {
	return !item.shareUrl.isEmpty()
		? item.shareUrl
		: u"https://t.me/%1/%2"_q.arg(item.username).arg(item.messageId.bare);
}

} // namespace

struct Widget::Playback {
	std::shared_ptr<Item> item;
	std::shared_ptr<Data::DocumentMedia> media;
	std::unique_ptr<::Media::Streaming::Instance> instance;
	bool withSound = false;
};

namespace {

struct Holder {
	std::shared_ptr<State> state;
	crl::time stateAt = 0;
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

// Android: SvipeReelWarmer — a warmed page is fresh for 10 minutes; the head is what plays first.
constexpr auto kFreshFor = crl::time(10 * 60 * 1000);
constexpr auto kWarmResolve = 3;
const auto kUsedKey = u"svipe_clips_used"_q;

void EnsureState(not_null<Window::SessionController*> controller, Holder &holder) {
	const auto stale = holder.state
		&& !holder.view
		&& (crl::now() - holder.stateAt > kFreshFor);
	if (!holder.state || stale) {
		holder.state = std::make_shared<State>();
		holder.state->feed = std::make_unique<Feed>(&controller->session());
		holder.stateAt = crl::now();
	}
}

void Warm(not_null<Window::SessionController*> controller) {
	if (!Storage::Get(&controller->session(), kUsedKey).toBool()) {
		return;
	}
	auto &holder = HolderFor(controller);
	EnsureState(controller, holder);
	const auto feed = holder.state->feed.get();
	const auto done = std::make_shared<bool>(false);
	const auto lifetime = std::make_shared<rpl::lifetime>();
	feed->updates() | rpl::on_next([=] {
		if (*done || feed->items().empty()) {
			return;
		}
		*done = true;
		const auto &items = feed->items();
		for (auto i = 0; i != std::min(int(items.size()), kWarmResolve); ++i) {
			feed->resolve(items[i], false, nullptr);
		}
		lifetime->destroy();
	}, *lifetime);
	feed->loadMore();
}

void Open(not_null<Window::SessionController*> controller) {
	controller->hideLayer(anim::type::instant); // the main menu Clips were opened from
	Video::Close(controller); // one full-window surface at a time
	Storage::Set(&controller->session(), kUsedKey, true);
	auto &holder = HolderFor(controller);
	EnsureState(controller, holder);
	if (holder.view) {
		holder.view->raise();
		holder.view->setFocus();
		return;
	}
	const auto body = controller->widget()->bodyWidget();
	holder.view = base::make_unique_q<Widget>(body, controller, holder.state);
	const auto view = holder.view.get();
	// Everything right of the folders sidebar: the sidebar stays, with Clips selected on it.
	rpl::combine(
		body->sizeValue(),
		rpl::single(rpl::empty) | rpl::then(controller->filtersMenuChanged())
	) | rpl::on_next([=](QSize size, auto) {
		const auto left = controller->hasFiltersMenu()
			? st::windowFiltersWidth
			: 0;
		view->setGeometry(QRect(left, 0, size.width() - left, size.height()));
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
	// What was under the layer has not painted since it went up: have the whole window repaint.
	Ui::ForceFullRepaint(controller->widget());
}

Widget::Widget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	std::shared_ptr<State> state)
: RpWidget(parent)
, _controller(controller)
, _state(std::move(state))
, _checkTimer([=] { checkPlaying(); })
, _singleClickTimer([=] { togglePause(); }) {
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);

	_state->feed->updates() | rpl::on_next([=] {
		if (!_playback && current()) {
			startPlayback();
		}
		update();
	}, lifetime());

	_checkTimer.callEach(kCheckPeriod);
	if (_state->feed->items().empty()) {
		_state->feed->loadMore();
	} else {
		startPlayback();
	}
}

Widget::~Widget() {
	stopPlayback();
}

void Widget::close() {
	// Destroys this widget: the holder owns it.
	Close(_controller);
}

bool Shown(not_null<Window::SessionController*> controller) {
	return HolderFor(controller).shown.current();
}

rpl::producer<bool> ShownValue(
		not_null<Window::SessionController*> controller) {
	return HolderFor(controller).shown.value();
}

std::shared_ptr<Item> Widget::at(int index) const {
	const auto &items = _state->feed->items();
	return (index >= 0 && index < int(items.size())) ? items[index] : nullptr;
}

std::shared_ptr<Item> Widget::current() const {
	return at(_state->index);
}

HistoryItem *Widget::currentMessage() const {
	const auto item = current();
	return (item && item->fullId)
		? controller()->session().data().message(item->fullId)
		: nullptr;
}

ChannelData *Widget::currentChannel() const {
	const auto item = current();
	if (!item) {
		return nullptr;
	} else if (const auto message = currentMessage()) {
		return message->history()->peer->asChannel();
	}
	const auto channel = controller()->session().data().channelLoaded(
		ChannelId(item->channelId));
	return (channel && !channel->name().isEmpty()) ? channel : nullptr;
}

void Widget::resizeEvent(QResizeEvent *e) {
	updateLayout();
	update();
}

void Widget::updateLayout() {
	// Instagram's desktop reels: a 9:16 card, the action rail beside it, arrows at the edge.
	const auto margin = S(16);
	const auto railWidth = S(48);
	const auto railGap = S(16);
	const auto arrow = S(48);
	const auto arrowsWidth = arrow + S(24) * 2;
	auto cardHeight = std::max(this->height() - 2 * margin, S(200));
	auto cardWidth = cardHeight * 9 / 16;
	const auto maxWidth = this->width()
		- 2 * (railGap + railWidth)
		- 2 * arrowsWidth;
	if (cardWidth > maxWidth && maxWidth > S(120)) {
		cardWidth = maxWidth;
		cardHeight = cardWidth * 16 / 9;
	}
	auto &l = _layout;
	l.card = QRect(
		(this->width() - cardWidth) / 2,
		(this->height() - cardHeight) / 2,
		cardWidth,
		cardHeight);

	// Instagram's "Messages" pill: the chats, waiting in the corner. Its width follows its content.
	const auto session = &controller()->session();
	const auto unread = session->data().unreadBadge();
	const auto badgeWidth = unread
		? std::max(S(18), st::semiboldFont->width(
			Lang::FormatCountToShort(unread).string) + S(8))
		: 0;
	const auto pillHeight = S(48);
	const auto labelWidth = S(12)
		+ st::semiboldFont->width(tr::lng_settings_messages(tr::now));
	const auto fullWidth = S(20) + st::menuIconChatDiscuss.width()
		+ (badgeWidth ? (badgeWidth - S(8)) : 0)
		+ labelWidth
		+ S(16) + S(26) + 2 * (S(26) - S(10)) + S(14);
	// A narrow window: the label goes, so the pill keeps off the card.
	_messagesCompact = (this->width() - S(24) - fullWidth)
		< (l.card.x() + l.card.width() + S(8));
	const auto pillWidth = fullWidth - (_messagesCompact ? labelWidth : 0);
	// With the folders sidebar beside Clips, All chats is already there: no pill.
	l.messages = controller()->hasFiltersMenu()
		? QRect()
		: QRect(
			this->width() - S(24) - pillWidth,
			this->height() - S(24) - pillHeight,
			pillWidth,
			pillHeight);

	const auto slot = S(56);
	const auto railX = l.card.x() + l.card.width() + railGap;
	auto railBottom = l.card.y() + l.card.height();
	if (!l.messages.isEmpty() && railX + railWidth > l.messages.x()) {
		// A narrow window: the rail climbs above the pill instead of hiding behind it.
		railBottom = std::min(railBottom, l.messages.y() - S(16));
	}
	auto y = railBottom - S(32);
	l.channel = QRect(railX + (railWidth - S(32)) / 2, y, S(32), S(32));
	y -= slot;
	l.more = QRect(railX, y, railWidth, slot - S(8));
	y -= slot;
	l.save = QRect(railX, y, railWidth, slot - S(8));
	y -= slot;
	l.share = QRect(railX, y, railWidth, slot - S(8));
	y -= slot + S(8);
	l.comments = QRect(railX, y, railWidth, slot);
	y -= slot + S(8);
	l.like = QRect(railX, y, railWidth, slot);

	const auto arrowX = this->width() - S(24) - arrow;
	const auto center = this->height() / 2;
	l.up = QRect(arrowX, center - S(8) - arrow, arrow, arrow);
	l.down = QRect(arrowX, center + S(8), arrow, arrow);
}

Widget::Button Widget::buttonAt(QPoint point) const {
	const auto &l = _layout;
	if (!l.messages.isEmpty() && l.messages.contains(point)) {
		return Button::Messages;
	} else if (!current()) {
		return Button::None;
	}
	const auto items = {
		std::pair{ l.like, Button::Like },
		std::pair{ l.comments, Button::Comments },
		std::pair{ l.share, Button::Share },
		std::pair{ l.save, Button::Save },
		std::pair{ l.more, Button::More },
		std::pair{ l.channel, Button::Channel },
		std::pair{ l.subscribe, Button::Subscribe },
		std::pair{ l.author, Button::Channel },
		std::pair{ l.up, Button::Up },
		std::pair{ l.down, Button::Down },
		std::pair{ l.messages, Button::Messages },
	};
	for (const auto &[rect, button] : items) {
		if (!rect.isEmpty() && rect.contains(point)) {
			if (button == Button::Up && _state->index <= 0) {
				return Button::None;
			}
			return button;
		}
	}
	return l.card.contains(point) ? Button::Card : Button::None;
}

void Widget::step(int delta) {
	const auto target = _state->index + delta;
	if (target < 0 || !at(target)) {
		if (delta > 0) {
			_state->feed->loadMore();
		}
		return;
	}
	_slideFrom = Ui::GrabWidget(this, _layout.card);
	_slideDirection = delta;
	stopPlayback();
	_state->index = target;
	_userPaused = false;
	_slide.stop();
	_slide.start([=] { update(); }, 0., 1., kSlideDuration, anim::easeOutCirc);
	startPlayback();
}

void Widget::startPlayback() {
	const auto item = current();
	if (!item) {
		return;
	}
	const auto &items = _state->feed->items();
	if (_state->index + kLoadMoreAhead >= int(items.size())) {
		_state->feed->loadMore();
	}
	refreshTexts();
	if (!item->document) {
		const auto weak = base::make_weak(this);
		_state->feed->resolve(item, true, [=] {
			if (!weak || current() != item) {
				return;
			} else if (item->document) {
				startPlayback();
			} else if (item->failed) {
				// Android's skip policy: the clip on screen has no route left — move on.
				step(1);
			}
			update();
		});
		update();
		return;
	}
	if (_playback && _playback->item == item) {
		return;
	}
	_playback = std::make_unique<Playback>();
	_playback->item = item;
	_playback->media = item->document->createMediaView();
	item->document->loadThumbnail(item->origin);
	_playback->withSound = !item->document->isSilentVideo()
		&& item->document->isVideoFile();
	_playback->instance = std::make_unique<::Media::Streaming::Instance>(
		item->document,
		item->origin,
		[=] { update(); });
	const auto instance = _playback->instance.get();
	instance->lockPlayer();
	instance->setPriority(kLoaderPriority);
	instance->player().updates(
	) | rpl::on_next_error([=](::Media::Streaming::Update &&update) {
		if (v::is<::Media::Streaming::Finished>(update.data)) {
			// Clips loop, as on Android; with sound the player stops at the end.
			if (_playback && _playback->instance.get() == instance) {
				play(0);
			}
		}
		this->update();
	}, [=](::Media::Streaming::Error &&error) {
		LOG(("Svipe Reels: %1/%2 playback error %3"
			).arg(item->username
			).arg(item->messageId.bare
			).arg(int(error)));
		if (_playback && _playback->instance.get() == instance) {
			item->failed = true;
			step(1);
		}
	}, instance->lifetime());

	_shownAt = crl::now();
	_watchStartedAt = 0;
	_watchedAccum = 0;
	_state->feed->sendEvent(*item, u"IMPRESSION"_q, QJsonObject{
		{ u"feed_position"_q, item->feedPosition },
	});
	play(0);
	checkPlaying();
	preloadAhead();
	update();
}

void Widget::play(crl::time position) {
	if (!_playback) {
		return;
	}
	const auto document = _playback->item->document;
	auto options = ::Media::Streaming::PlaybackOptions{
		.position = position,
		.hwAllowed = Core::App().settings().hardwareAcceleratedVideo(),
	};
	if (_playback->withSound) {
		options.audioId = AudioMsgId(document, _playback->item->fullId);
		Media::Player::instance()->pause(AudioMsgId::Type::Voice);
		Media::Player::instance()->pause(AudioMsgId::Type::Song);
		Media::Player::mixer()->setVideoVolume(
			Core::App().settings().videoVolume());
	} else {
		options.mode = ::Media::Streaming::Mode::Video;
		options.loop = true;
	}
	_playback->instance->play(options);
}

void Widget::stopPlayback() {
	if (!_playback) {
		return;
	}
	const auto playback = base::take(_playback);
	const auto item = playback->item;
	// Android's flushWatchEvent: the one event the recommender learns the most from.
	if (_shownAt) {
		const auto now = crl::now();
		const auto watched = _watchedAccum
			+ (_watchStartedAt ? (now - _watchStartedAt) : 0);
		const auto dwell = now - _shownAt;
		auto duration = (item->document && item->document->hasDuration())
			? item->document->duration()
			: item->durationMs;
		if (duration <= 0) {
			const auto &track = playback->instance->info().video.state;
			if (track.duration > 0) {
				duration = track.duration;
			}
		}
		const auto type = Feed::Classify(watched, duration);
		auto payload = QJsonObject{
			{ u"watched_ms"_q, qint64(watched) },
			{ u"dwell_ms"_q, qint64(dwell) },
			{ u"feed_position"_q, item->feedPosition },
		};
		if (duration > 0) {
			payload.insert(u"video_duration_ms"_q, qint64(duration));
		}
		_state->feed->sendEvent(*item, type, payload);
		if (type != u"SWIPE_AWAY"_q || watched >= kMinWatched) {
			_state->feed->markWatched(*item);
		}
	}
	_shownAt = _watchStartedAt = _watchedAccum = 0;
	playback->instance->stop();
}

void Widget::checkPlaying() {
	if (!_playback) {
		return;
	}
	const auto instance = _playback->instance.get();
	const auto window = controller()->widget();
	const auto allowed = isVisible()
		&& !_userPaused
		&& !_menu
		&& window->isActive()
		&& !controller()->isLayerShown();
	if (allowed && instance->paused()) {
		instance->resume();
	} else if (!allowed && instance->active() && !instance->paused()) {
		instance->pause();
	}
	const auto playing = allowed
		&& instance->ready()
		&& !instance->waitingShown();
	const auto now = crl::now();
	if (playing && !_watchStartedAt) {
		_watchStartedAt = now;
	} else if (!playing && _watchStartedAt) {
		_watchedAccum += now - _watchStartedAt;
		_watchStartedAt = 0;
	}
	update(); // the progress line, and counts that changed on the server
}

void Widget::preloadAhead() {
	for (auto i = 1; i <= kResolveAhead; ++i) {
		const auto item = at(_state->index + i);
		if (!item || item->document || item->failed || item->resolving) {
			continue;
		}
		const auto weak = base::make_weak(this);
		_state->feed->resolve(item, false, [=] {
			// Never re-enter synchronously: a resolved clip answers at once.
			if (weak && item->document) {
				crl::on_main(weak, [=] { preloadAhead(); });
			}
		});
	}
	const auto next = at(_state->index + 1);
	const auto document = next ? next->document : nullptr;
	if (document == _preloading) {
		return;
	}
	_preload = nullptr;
	_preloading = nullptr;
	if (document && Data::VideoPreload::Can(document)) {
		_preloading = document;
		_preload = std::make_unique<Data::VideoPreload>(
			document,
			next->origin,
			[] {});
	}
}

void Widget::refreshTexts() {
	const auto item = current();
	const auto channel = currentChannel();
	_title.setText(
		st::semiboldTextStyle,
		!item
			? QString()
			: channel
			? channel->name()
			: !item->title.isEmpty()
			? item->title
			: (u"@"_q + item->username));
	_caption.setMarkedText(
		st::defaultTextStyle,
		item ? item->caption : TextWithEntities());
	_userpic = {};
}

void Widget::togglePause() {
	_userPaused = !_userPaused;
	checkPlaying();
	update();
}

void Widget::withMessage(Fn<void(not_null<HistoryItem*>)> callback) {
	const auto item = current();
	if (!item) {
		return;
	}
	const auto weak = base::make_weak(this);
	_state->feed->requireMessage(item, [=](HistoryItem *message) {
		if (!weak || current() != item) {
			return;
		} else if (!message) {
			controller()->showToast(Tr(Str::ReelsActionUnavailable));
			return;
		}
		refreshTexts();
		callback(message);
		update();
	});
}

void Widget::activate(Button button) {
	switch (button) {
	case Button::Like: toggleLike(); break;
	case Button::Comments: openComments(); break;
	case Button::Share: share(); break;
	case Button::Save: save(); break;
	case Button::More: showMore(); break;
	case Button::Channel: goToChannel(); break;
	case Button::Subscribe: subscribe(); break;
	case Button::Up: step(-1); break;
	case Button::Down: step(1); break;
	case Button::Messages: close(); break;
	case Button::Card:
		// A tap pauses; a second tap within the double-click interval is a like instead.
		_singleClickTimer.callOnce(QGuiApplication::styleHints()->mouseDoubleClickInterval());
		break;
	case Button::None: break;
	}
}

void Widget::toggleLike() {
	const auto item = current();
	withMessage([=](not_null<HistoryItem*> message) {
		const auto heart = Data::ReactionId{ kLikeEmoji };
		const auto liked = ranges::contains(
			message->chosenReactions(),
			heart);
		message->toggleReaction(heart, HistoryReactionSource::Selector);
		_state->feed->sendEvent(*item, liked ? u"UNLIKE"_q : u"LIKE"_q);
	});
}

void Widget::openComments() {
	withMessage([=](not_null<HistoryItem*> message) {
		const auto controller = this->controller();
		const auto history = message->history();
		const auto id = message->id;
		close();
		controller->showRepliesForMessage(history, id);
	});
}

void Widget::share() {
	const auto item = current();
	if (!item) {
		return;
	}
	// Android's share: the video itself as a clean message (no "forwarded from"), carrying the
	// promo line and the link with the scheme stripped.
	const auto link = ShareLink(*item);
	auto stripped = link;
	stripped.remove(QRegularExpression(u"^https?://"_q));
	const auto caption = u"watch clips on telegram with svipe\n\n"_q + stripped;
	const auto document = item->document;
	const auto session = &controller()->session();
	Window::ShowChooseRecipientBox(controller(), [=](
			not_null<Data::Thread*> thread) {
		auto message = Api::MessageToSend(Api::SendAction(thread));
		message.textWithTags = { caption };
		if (document) {
			Api::SendExistingDocument(std::move(message), document);
		} else {
			session->api().sendMessage(std::move(message));
		}
		return true;
	});
	_state->feed->sendEvent(*item, u"SHARE"_q);
}

void Widget::save() {
	withMessage([=](not_null<HistoryItem*> message) {
		const auto weak = base::make_weak(this);
		_state->feed->save(message, [=](ChannelData *channel) {
			if (!weak) {
				return;
			}
			controller()->showToast(channel
				? (Tr(Str::ReelsSavedChannel) + u" ✓"_q)
				: Tr(Str::ReelsActionUnavailable));
		});
	});
}

void Widget::showMore() {
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(Tr(Str::ReelsShare), [=] { share(); }, &st::menuIconShare);
	_menu->addAction(
		tr::lng_context_copy_link(tr::now),
		[=] { copyLink(); },
		&st::menuIconCopy);
	_menu->addAction(
		Tr(Str::ReelsGoToChannel),
		[=] { goToChannel(); },
		&st::menuIconChannel);
	_menu->addAction(
		Tr(Str::ReelsNotInterested),
		[=] { notInterested(); },
		&st::menuIconBlock);
	_menu->addAction(
		Tr(Str::ReelsBlockChannel),
		[=] { blockChannel(); },
		&st::menuIconBlockAttention);
	_menu->addAction(
		tr::lng_report_button(tr::now),
		[=] { report(); },
		&st::menuIconReport);
	_menu->setDestroyedCallback(crl::guard(this, [=] {
		checkPlaying();
		update();
	}));
	_menu->popup(QCursor::pos());
	checkPlaying();
}

void Widget::copyLink() {
	if (const auto item = current()) {
		QGuiApplication::clipboard()->setText(ShareLink(*item));
		controller()->showToast(tr::lng_channel_public_link_copied(tr::now));
	}
}

void Widget::goToChannel() {
	withMessage([=](not_null<HistoryItem*> message) {
		const auto controller = this->controller();
		const auto peer = message->history()->peer;
		const auto id = message->id;
		close();
		controller->showPeerHistory(
			peer,
			Window::SectionShow::Way::ClearStack,
			id);
	});
}

void Widget::notInterested() {
	if (const auto item = current()) {
		_state->feed->sendEvent(*item, u"NOT_INTERESTED"_q);
		controller()->showToast(Tr(Str::ReelsLessLikeThis));
	}
}

void Widget::blockChannel() {
	const auto item = current();
	if (!item) {
		return;
	}
	const auto channelId = item->channelId;
	stopPlayback();
	_state->feed->sendEvent(*item, u"BLOCK_CHANNEL"_q);
	// The clips before this one stay where they were; the rest close up.
	auto before = 0;
	for (auto i = 0; i < _state->index; ++i) {
		if (at(i)->channelId == channelId) {
			++before;
		}
	}
	_state->feed->block(channelId);
	_state->index = std::clamp(
		_state->index - before,
		0,
		std::max(int(_state->feed->items().size()) - 1, 0));
	controller()->showToast(Tr(Str::ReelsChannelBlocked));
	startPlayback();
	if (!current()) {
		_state->feed->loadMore();
	}
	update();
}

void Widget::report() {
	withMessage([=](not_null<HistoryItem*> message) {
		ShowReportMessageBox(
			controller()->uiShow(),
			message->history()->peer,
			{ message->id },
			{});
	});
}

void Widget::subscribe() {
	const auto item = current();
	withMessage([=](not_null<HistoryItem*> message) {
		if (const auto channel = message->history()->peer->asChannel()) {
			if (!channel->amIn()) {
				channel->session().api().joinChannel(channel);
				_state->feed->sendEvent(*item, u"FOLLOW"_q);
				controller()->showToast(Tr(Str::ReelsSubscribed));
			}
		}
	});
}

void Widget::paintCard(Painter &p, const Layout &l) {
	const auto item = current();
	const auto radius = S(8);
	auto clip = QPainterPath();
	clip.addRoundedRect(l.card, radius, radius);
	p.save();
	p.setClipPath(clip);
	p.fillRect(l.card, QColor(0, 0, 0));
	if (!item) {
		const auto status = _state->feed->status();
		if (!status.isEmpty()) {
			p.setPen(QColor(255, 255, 255, 180));
			p.setFont(st::normalFont);
			p.drawText(
				l.card.marginsRemoved({ S(16), 0, S(16), 0 }),
				Qt::AlignCenter | Qt::TextWordWrap,
				status);
		}
		p.restore();
		return;
	}
	const auto instance = (_playback && _playback->item == item)
		? _playback->instance.get()
		: nullptr;
	const auto ready = instance
		&& instance->player().ready()
		&& !instance->player().videoSize().isEmpty();
	if (ready) {
		const auto target = FitInto(instance->player().videoSize(), l.card);
		const auto ratio = style::DevicePixelRatio();
		const auto frame = instance->frame({
			.resize = target.size() * ratio,
			.outer = target.size() * ratio,
		});
		p.drawImage(target, frame);
		instance->markFrameShown();
	} else if (_playback && _playback->item == item) {
		const auto media = _playback->media.get();
		const auto image = media->thumbnail()
			? media->thumbnail()
			: media->thumbnailInline();
		if (image) {
			const auto size = item->document->dimensions.isEmpty()
				? image->size()
				: item->document->dimensions;
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(FitInto(size, l.card), image->original());
		}
	}

	// The bottom of the card: the channel with Subscribe, then the caption.
	const auto padding = S(16);
	const auto available = l.card.width() - 2 * padding;
	const auto userpicSize = S(32);
	const auto captionLines = 2;
	const auto captionHeight = _caption.isEmpty()
		? 0
		: (std::min(
			_caption.countHeight(available),
			st::normalFont->height * captionLines) + S(8));
	const auto block = userpicSize + captionHeight + padding * 2;
	auto gradient = QLinearGradient(
		0,
		l.card.y() + l.card.height() - block * 2,
		0,
		l.card.y() + l.card.height());
	gradient.setColorAt(0., QColor(0, 0, 0, 0));
	gradient.setColorAt(1., QColor(0, 0, 0, 170));
	p.fillRect(
		QRect(
			l.card.x(),
			l.card.y() + l.card.height() - block * 2,
			l.card.width(),
			block * 2),
		gradient);

	auto top = l.card.y() + l.card.height() - block + padding;
	auto left = l.card.x() + padding;
	const auto channel = currentChannel();
	if (channel) {
		channel->paintUserpic(p, _userpic, left, top, userpicSize);
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 60));
		auto hq = PainterHighQualityEnabler(p);
		p.drawEllipse(QRect(left, top, userpicSize, userpicSize));
	}
	left += userpicSize + S(10);
	const auto nameTop = top + (userpicSize - st::semiboldFont->height) / 2;
	const auto subscribeText = (channel && !channel->amIn())
		? (u" • "_q + Tr(Str::ReelsSubscribe))
		: QString();
	const auto subscribeWidth = subscribeText.isEmpty()
		? 0
		: st::semiboldFont->width(subscribeText);
	const auto nameAvailable = l.card.x() + l.card.width() - padding
		- left
		- subscribeWidth;
	const auto nameWidth = std::min(_title.maxWidth(), nameAvailable);
	p.setPen(QColor(255, 255, 255));
	_title.drawElided(p, left, nameTop, nameWidth);
	_layout.author = QRect(
		l.card.x() + padding,
		top,
		left - l.card.x() - padding + nameWidth,
		userpicSize);
	if (subscribeWidth) {
		p.setFont(st::semiboldFont);
		p.setPen(kLinkColor);
		p.drawText(left + nameWidth, nameTop + st::semiboldFont->ascent, subscribeText);
		_layout.subscribe = QRect(
			left + nameWidth,
			top,
			subscribeWidth,
			userpicSize);
	} else {
		_layout.subscribe = QRect();
	}
	top += userpicSize + S(8);
	if (!_caption.isEmpty()) {
		p.setPen(QColor(255, 255, 255, 230));
		_caption.drawElided(p, l.card.x() + padding, top, available, captionLines);
	}

	// Progress, a thin line along the bottom of the card.
	if (ready) {
		const auto &track = instance->info().video.state;
		if (track.duration > 0 && track.position != ::Media::kTimeUnknown) {
			const auto progress = std::clamp(
				track.position / float64(track.duration),
				0.,
				1.);
			const auto line = std::max(S(2), 1);
			p.fillRect(
				QRect(
					l.card.x(),
					l.card.y() + l.card.height() - line,
					int(l.card.width() * progress),
					line),
				QColor(255, 255, 255, 200));
		}
	}

	// Paused: a centered play glyph.
	if (_userPaused) {
		const auto size = S(64);
		const auto center = l.card.center();
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 110));
		p.drawEllipse(QRect(
			center.x() - size / 2,
			center.y() - size / 2,
			size,
			size));
		auto path = QPainterPath();
		const auto side = size * 0.36;
		path.moveTo(center.x() - side * 0.4, center.y() - side / 2);
		path.lineTo(center.x() - side * 0.4, center.y() + side / 2);
		path.lineTo(center.x() + side * 0.6, center.y());
		path.closeSubpath();
		p.setBrush(QColor(255, 255, 255));
		p.drawPath(path);
	}
	p.restore();
}

void Widget::paintRail(Painter &p, const Layout &l) {
	if (!current()) {
		return;
	}
	const auto message = currentMessage();
	auto likes = 0;
	auto liked = false;
	if (message) {
		for (const auto &reaction : message->reactions()) {
			likes += reaction.count;
			liked = liked || reaction.my;
		}
	}
	const auto comments = message ? std::max(message->repliesCount(), 0) : 0;
	const auto white = QColor(255, 255, 255);
	const auto hovered = [&](Button button) {
		return (_over == button) ? QColor(255, 255, 255, 190) : white;
	};
	const auto iconSize = S(24);
	const auto iconRect = [&](const QRect &slot) {
		return QRect(
			slot.x() + (slot.width() - iconSize) / 2,
			slot.y(),
			iconSize,
			iconSize);
	};
	const auto count = [&](const QRect &slot, int value) {
		if (value <= 0) {
			return;
		}
		p.setFont(st::normalFont);
		p.setPen(white);
		p.drawText(
			QRect(slot.x() - S(8), slot.y() + iconSize + S(4), slot.width() + S(16), st::normalFont->height),
			Qt::AlignHCenter | Qt::AlignTop,
			Lang::FormatCountToShort(value).string);
	};
	const auto icon = [&](const style::icon &icon, const QRect &slot, Button button) {
		const auto r = iconRect(slot);
		icon.paint(
			p,
			QPoint(
				r.x() + (r.width() - icon.width()) / 2,
				r.y() + (r.height() - icon.height()) / 2),
			width(),
			hovered(button));
	};

	{
		auto hq = PainterHighQualityEnabler(p);
		const auto r = QRectF(iconRect(l.like)).marginsRemoved(
			QMarginsF(S(1), S(2), S(1), S(1)));
		const auto heart = HeartPath(r);
		if (liked) {
			p.setPen(Qt::NoPen);
			p.setBrush(kLikedColor);
		} else {
			p.setPen(QPen(hovered(Button::Like), S(2)));
			p.setBrush(Qt::NoBrush);
		}
		p.drawPath(heart);
	}
	count(l.like, likes);
	icon(st::menuIconChatDiscuss, l.comments, Button::Comments);
	count(l.comments, comments);
	icon(st::menuIconShare, l.share, Button::Share);
	icon(st::menuIconSavedMessages, l.save, Button::Save);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(hovered(Button::More));
		const auto r = iconRect(l.more);
		const auto dot = S(4);
		for (auto i = -1; i <= 1; ++i) {
			p.drawEllipse(QRect(
				r.center().x() + i * S(7) - dot / 2,
				r.center().y() - dot / 2,
				dot,
				dot));
		}
	}
	if (const auto channel = currentChannel()) {
		auto view = _userpic;
		channel->paintUserpic(
			p,
			view,
			l.channel.x(),
			l.channel.y(),
			l.channel.width());
	} else {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 60));
		p.drawEllipse(l.channel);
	}
}

void Widget::paintArrows(Painter &p, const Layout &l) {
	if (!current()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto paint = [&](const QRect &r, bool up, bool enabled, Button button) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, (_over == button) ? 46 : 26));
		p.drawEllipse(r);
		auto pen = QPen(QColor(255, 255, 255, enabled ? 230 : 70), S(2));
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto c = QPointF(r.center()) + QPointF(0.5, 0.5);
		const auto w = S(8);
		const auto h = S(4) * (up ? -1 : 1);
		auto path = QPainterPath();
		path.moveTo(c.x() - w, c.y() - h);
		path.lineTo(c.x(), c.y() + h);
		path.lineTo(c.x() + w, c.y() - h);
		p.drawPath(path);
	};
	paint(l.up, true, _state->index > 0, Button::Up);
	paint(l.down, false, at(_state->index + 1) != nullptr, Button::Down);
}

void Widget::paintEvent(QPaintEvent *e) {
	updateLayout(); // the pill follows the unread count
	auto p = Painter(this);
	// The same background as the folders sidebar beside it, in every theme.
	p.fillRect(rect(), st::windowFiltersButton.textBg);
	const auto progress = _slide.value(1.);
	if (progress < 1. && !_slideFrom.isNull()) {
		const auto &card = _layout.card;
		p.save();
		p.setClipRect(card);
		const auto shift = int(card.height() * progress) * _slideDirection;
		p.drawPixmap(card.x(), card.y() - shift, _slideFrom);
		p.translate(0, _slideDirection * card.height() - shift);
		paintCard(p, _layout);
		p.restore();
	} else {
		_slideFrom = QPixmap();
		paintCard(p, _layout);
	}
	paintRail(p, _layout);
	paintArrows(p, _layout);
	paintMessages(p, _layout);
}

void Widget::paintMessages(Painter &p, const Layout &l) {
	const auto &r = l.messages;
	if (r.isEmpty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x26, 0x26, 0x26, (_over == Button::Messages) ? 255 : 235));
	p.drawRoundedRect(r, r.height() / 2., r.height() / 2.);

	const auto session = &controller()->session();
	auto left = r.x() + S(20);
	const auto icon = &st::menuIconChatDiscuss;
	icon->paint(
		p,
		QPoint(left, r.y() + (r.height() - icon->height()) / 2),
		width(),
		QColor(255, 255, 255));
	auto labelLeft = left + icon->width() + S(12);
	if (const auto unread = session->data().unreadBadge()) {
		const auto text = Lang::FormatCountToShort(unread).string;
		const auto font = st::semiboldFont;
		const auto h = S(18);
		const auto w = std::max(h, font->width(text) + S(8));
		const auto badge = QRect(left + icon->width() - S(8), r.y() + S(4), w, h);
		p.setPen(Qt::NoPen);
		p.setBrush(kLikedColor);
		p.drawRoundedRect(badge, h / 2., h / 2.);
		p.setFont(font);
		p.setPen(QColor(255, 255, 255));
		p.drawText(badge, Qt::AlignCenter, text);
		labelLeft = std::max(labelLeft, badge.x() + badge.width() + S(4));
	}
	left = labelLeft;
	if (!_messagesCompact) {
		p.setFont(st::semiboldFont);
		p.setPen(QColor(255, 255, 255));
		p.drawText(
			QRect(left, r.y(), r.width(), r.height()),
			Qt::AlignVCenter | Qt::AlignLeft,
			tr::lng_settings_messages(tr::now));
	}

	// The last chats, overlapping, at the pill's right end.
	auto peers = std::vector<not_null<PeerData*>>();
	for (const auto &row : session->data().chatsList()->indexed()->all()) {
		if (const auto history = row->history()) {
			peers.push_back(history->peer);
			if (peers.size() == _recentUserpics.size()) {
				break;
			}
		}
	}
	const auto size = S(26);
	auto x = r.x() + r.width() - S(14) - size;
	for (auto i = int(peers.size()); i > 0; --i) {
		const auto index = i - 1;
		const auto rx = x - (int(peers.size()) - i) * (size - S(10));
		p.setBrush(QColor(0x26, 0x26, 0x26));
		p.setPen(Qt::NoPen);
		p.drawEllipse(QRect(rx - S(2), r.y() + (r.height() - size) / 2 - S(2), size + S(4), size + S(4)));
		peers[index]->paintUserpic(
			p,
			_recentUserpics[index],
			rx,
			r.y() + (r.height() - size) / 2,
			size);
	}
}

void Widget::wheelEvent(QWheelEvent *e) {
	e->accept();
	if (e->phase() == Qt::ScrollMomentum) {
		return; // the swipe already paged; its inertia must not page again
	}
	const auto now = crl::now();
	if (now < _wheelBlockedTill || _slide.animating()) {
		_wheelAccumulated = 0;
		return;
	}
	if (now - _wheelLastAt > kWheelIdleReset) {
		_wheelAccumulated = 0;
	}
	_wheelLastAt = now;
	const auto pixel = e->pixelDelta().y();
	const auto delta = pixel ? pixel : e->angleDelta().y();
	_wheelAccumulated += delta;
	const auto threshold = pixel
		? S(48)
		: QWheelEvent::DefaultDeltasPerStep;
	if (std::abs(_wheelAccumulated) >= threshold) {
		const auto direction = (_wheelAccumulated < 0) ? 1 : -1;
		_wheelAccumulated = 0;
		_wheelBlockedTill = now + kWheelBlock;
		step(direction);
	}
}

void Widget::keyPressEvent(QKeyEvent *e) {
	switch (e->key()) {
	case Qt::Key_Down:
	case Qt::Key_PageDown:
	case Qt::Key_J:
		step(1);
		return;
	case Qt::Key_Up:
	case Qt::Key_PageUp:
	case Qt::Key_K:
		step(-1);
		return;
	case Qt::Key_Space:
		togglePause();
		return;
	case Qt::Key_L:
		toggleLike();
		return;
	case Qt::Key_Escape:
		close();
		return;
	}
	RpWidget::keyPressEvent(e);
}

void Widget::mousePressEvent(QMouseEvent *e) {
	setFocus();
	if (e->button() == Qt::LeftButton) {
		_pressed = buttonAt(e->pos());
		_pressedAt = e->pos();
	}
}

void Widget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, Button::None);
	if (pressed != Button::None && buttonAt(e->pos()) == pressed) {
		activate(pressed);
	}
}

void Widget::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && buttonAt(e->pos()) == Button::Card) {
		// Android's double tap: a like, never an unlike.
		_singleClickTimer.cancel();
		const auto message = currentMessage();
		const auto heart = Data::ReactionId{ kLikeEmoji };
		if (!message || !ranges::contains(message->chosenReactions(), heart)) {
			toggleLike();
		}
		return;
	}
	mousePressEvent(e);
}

void Widget::mouseMoveEvent(QMouseEvent *e) {
	const auto over = buttonAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over == Button::None || over == Button::Card)
			? style::cur_default
			: style::cur_pointer);
		update();
	}
}

void Widget::leaveEventHook(QEvent *e) {
	if (_over != Button::None) {
		_over = Button::None;
		setCursor(style::cur_default);
		update();
	}
}

} // namespace Svipe::Reels
