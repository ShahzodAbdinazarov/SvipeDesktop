/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The Clips screen (Android: ReelsActivity), laid out the way Instagram shows reels on desktop: one
9:16 card in the middle of the column, the channel and caption on the card, the actions in a rail to
its right (like, comments, share, save, more, the channel), and up / down buttons at the right edge.
Paged with the mouse wheel, the touchpad, the arrow keys or those buttons; played with Telegram's own
streaming player, the next clip preloaded. Opened from the main menu; it keeps its place in the feed
when you come back to it.
*/
#pragma once

#include "window/section_widget.h"
#include "window/section_memento.h"
#include "ui/effects/animations.h"
#include "ui/text/text.h"
#include "ui/userpic_view.h"
#include "base/timer.h"
#include "base/unique_qptr.h"

class Painter;
class HistoryItem;
class ChannelData;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Data {
class MediaPreload;
class DocumentMedia;
} // namespace Data

namespace Media::Streaming {
class Instance;
} // namespace Media::Streaming

namespace Svipe::Reels {

class Feed;
struct Item;

struct State {
	std::unique_ptr<Feed> feed;
	int index = 0;
};

void Open(not_null<Window::SessionController*> controller);

class Memento final : public Window::SectionMemento {
public:
	explicit Memento(std::shared_ptr<State> state);

	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override;

	[[nodiscard]] const std::shared_ptr<State> &state() const {
		return _state;
	}

private:
	std::shared_ptr<State> _state;

};

class Widget final : public Window::SectionWidget {
public:
	Widget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<State> state);
	~Widget();

	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;
	QPixmap grabForShowAnimation(
		const Window::SectionSlideParams &params) override;

	bool floatPlayerHandleWheelEvent(QEvent *e) override;
	QRect floatPlayerAvailableRect() override;

private:
	struct Playback;
	enum class Button {
		None,
		Like,
		Comments,
		Share,
		Save,
		More,
		Channel,
		Subscribe,
		Up,
		Down,
		Card,
	};
	struct Layout {
		QRect card;
		QRect like;
		QRect comments;
		QRect share;
		QRect save;
		QRect more;
		QRect channel;
		QRect author;
		QRect subscribe;
		QRect up;
		QRect down;
	};

	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void doSetInnerFocus() override;
	void showFinishedHook() override;

	[[nodiscard]] std::shared_ptr<Item> current() const;
	[[nodiscard]] std::shared_ptr<Item> at(int index) const;
	[[nodiscard]] HistoryItem *currentMessage() const;
	[[nodiscard]] ChannelData *currentChannel() const;
	[[nodiscard]] Button buttonAt(QPoint point) const;
	void updateLayout();
	void step(int delta);
	void startPlayback();
	void stopPlayback();
	void play(crl::time position);
	void checkPlaying();
	void preloadAhead();
	void refreshTexts();
	void togglePause();

	void activate(Button button);
	void withMessage(Fn<void(not_null<HistoryItem*>)> callback);
	void toggleLike();
	void openComments();
	void share();
	void save();
	void showMore();
	void copyLink();
	void goToChannel();
	void notInterested();
	void blockChannel();
	void report();
	void subscribe();

	void paintCard(Painter &p, const Layout &layout);
	void paintRail(Painter &p, const Layout &layout);
	void paintArrows(Painter &p, const Layout &layout);

	const std::shared_ptr<State> _state;
	std::unique_ptr<Playback> _playback;
	std::unique_ptr<Data::MediaPreload> _preload;
	DocumentData *_preloading = nullptr;
	Layout _layout;

	Ui::Text::String _title;
	Ui::Text::String _caption;
	Ui::PeerUserpicView _userpic;

	bool _userPaused = false;
	base::Timer _checkTimer;
	base::Timer _singleClickTimer;
	base::unique_qptr<Ui::PopupMenu> _menu;

	// Watch clock of the clip on screen (Android: itemShownMs / watchStartMs / watchedAccumMs).
	crl::time _shownAt = 0;
	crl::time _watchStartedAt = 0;
	crl::time _watchedAccum = 0;

	Ui::Animations::Simple _slide;
	QPixmap _slideFrom;
	int _slideDirection = 0;

	int _wheelAccumulated = 0;
	crl::time _wheelBlockedTill = 0;
	crl::time _wheelLastAt = 0;
	QPoint _pressedAt;
	Button _pressed = Button::None;
	Button _over = Button::None;

};

} // namespace Svipe::Reels
