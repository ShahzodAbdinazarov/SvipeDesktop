/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A chat's "Recent actions": its deleted and edited messages, drawn the way Telegram draws a channel's
admin log (Android: SvipeDeletedLogActivity). The rows are the admin log's own pieces — a service
line "X deleted message:" / "X edited message:", then the message as an AdminLogEntry item, an edit
carrying the "Original message" block — shown in the ListWidget that the pinned and scheduled
sections use, so nothing here re-implements message rendering.
*/
#pragma once

#include "window/section_widget.h"
#include "window/section_memento.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_corner_buttons.h"

class History;

namespace Ui {
class ElasticScroll;
class PlainShadow;
class SettingsButton;
} // namespace Ui

namespace HistoryView {
class TopBarWidget;
} // namespace HistoryView

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Svipe {

class RecentActionsMemento;

// The chat menu's "Recent actions" entry (Android: the EventLog item of the chat's ⋮ menu).
void AddRecentActionsAction(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread,
	const Ui::Menu::MenuCallback &addAction);

class RecentActionsWidget final
	: public Window::SectionWidget
	, private HistoryView::WindowListDelegate
	, private HistoryView::CornerButtonsDelegate {
public:
	RecentActionsWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<History*> history);
	~RecentActionsWidget();

	[[nodiscard]] not_null<History*> history() const {
		return _history;
	}
	Dialogs::RowDescriptor activeChat() const override;
	bool hasTopBarShadow() const override {
		return true;
	}

	QPixmap grabForShowAnimation(
		const Window::SectionSlideParams &params) override;

	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;

	void setInternalState(
		const QRect &geometry,
		not_null<RecentActionsMemento*> memento);

	bool floatPlayerHandleWheelEvent(QEvent *e) override;
	QRect floatPlayerAvailableRect() override;

	// ListDelegate interface.
	HistoryView::Context listContext() override;
	bool listScrollTo(int top, bool syntetic = true) override;
	void listCancelRequest() override;
	void listDeleteRequest() override;
	void listTryProcessKeyInput(not_null<QKeyEvent*> e) override;
	rpl::producer<Data::MessagesSlice> listSource(
		Data::MessagePosition aroundId,
		int limitBefore,
		int limitAfter) override;
	bool listAllowsMultiSelect() override;
	bool listIsItemGoodForSelection(not_null<HistoryItem*> item) override;
	bool listIsLessInOrder(
		not_null<HistoryItem*> first,
		not_null<HistoryItem*> second) override;
	void listSelectionChanged(HistoryView::SelectedItems &&items) override;
	void listMarkReadTill(not_null<HistoryItem*> item) override;
	void listMarkContentsRead(
		const base::flat_set<not_null<HistoryItem*>> &items) override;
	HistoryView::MessagesBarData listMessagesBar(
		const std::vector<not_null<HistoryView::Element*>> &elements,
		bool markLastAsRead) override;
	void listContentRefreshed() override;
	void listUpdateDateLink(
		ClickHandlerPtr &link,
		not_null<HistoryView::Element*> view) override;
	bool listElementHideReply(
		not_null<const HistoryView::Element*> view) override;
	bool listElementShownUnread(
		not_null<const HistoryView::Element*> view) override;
	bool listIsGoodForAroundPosition(
		not_null<const HistoryView::Element*> view) override;
	void listSendBotCommand(
		const QString &command,
		const FullMsgId &context) override;
	void listSearch(
		const QString &query,
		const FullMsgId &context) override;
	void listHandleViaClick(not_null<UserData*> bot) override;
	not_null<Ui::ChatTheme*> listChatTheme() override;
	HistoryView::CopyRestrictionType listCopyRestrictionType(
		HistoryItem *item) override;
	HistoryView::CopyRestrictionType listCopyMediaRestrictionType(
		not_null<HistoryItem*> item) override;
	HistoryView::CopyRestrictionType listSelectRestrictionType() override;
	auto listAllowedReactionsValue()
		-> rpl::producer<Data::AllowedReactions> override;
	void listShowPremiumToast(not_null<DocumentData*> document) override;
	void listOpenPhoto(
		not_null<PhotoData*> photo,
		FullMsgId context) override;
	void listOpenDocument(
		not_null<DocumentData*> document,
		FullMsgId context,
		bool showInMediaView) override;
	void listPaintEmpty(
		Painter &p,
		const Ui::ChatPaintContext &context) override;
	QString listElementAuthorRank(
		not_null<const HistoryView::Element*> view) override;
	bool listElementHideTopicButton(
		not_null<const HistoryView::Element*> view) override;
	History *listTranslateHistory() override;
	void listAddTranslatedItems(
		not_null<HistoryView::TranslateTracker*> tracker) override;
	Ui::ElasticScroll *listScrollArea() const override;
	bool listThanosEffectEnabled() const override;

	// CornerButtonsDelegate interface.
	void cornerButtonsShowAtPosition(
		Data::MessagePosition position) override;
	Data::Thread *cornerButtonsThread() override;
	FullMsgId cornerButtonsCurrentId() override;
	bool cornerButtonsIgnoreVisibility() override;
	std::optional<bool> cornerButtonsDownShown() override;
	bool cornerButtonsUnreadMayBeShown() override;
	bool cornerButtonsHas(HistoryView::CornerButtonType type) override;

private:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showAnimatedHook(
		const Window::SectionSlideParams &params) override;
	void showFinishedHook() override;
	void doSetInnerFocus() override;
	void checkActivation() override;

	void onScroll();
	void updateInnerVisibleArea();
	void updateControlsGeometry();
	void updateAdaptiveLayout();
	void recountChatWidth();

	void rebuild();
	void clearItems();
	[[nodiscard]] Data::MessagesSlice slice() const;

	const not_null<History*> _history;
	std::shared_ptr<Ui::ChatTheme> _theme;
	QPointer<HistoryView::ListWidget> _inner;
	object_ptr<HistoryView::TopBarWidget> _topBar;
	object_ptr<Ui::PlainShadow> _topBarShadow;
	// Android's "Show in chat" switch at the top of the screen.
	std::unique_ptr<Ui::SettingsButton> _showInChat;
	bool _skipScrollEvent = false;
	std::unique_ptr<Ui::ElasticScroll> _scroll;
	HistoryView::CornerButtons _cornerButtons;

	// The log rows, oldest first; they are AdminLogEntry items this widget owns.
	std::vector<not_null<HistoryItem*>> _items;
	base::flat_map<not_null<HistoryItem*>, int> _order;
	rpl::event_stream<> _rebuilt;

};

class RecentActionsMemento final : public Window::SectionMemento {
public:
	explicit RecentActionsMemento(not_null<History*> history);

	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override;

	[[nodiscard]] not_null<History*> getHistory() const {
		return _history;
	}
	[[nodiscard]] not_null<HistoryView::ListMemento*> list() {
		return &_list;
	}

private:
	const not_null<History*> _history;
	HistoryView::ListMemento _list;

};

} // namespace Svipe
