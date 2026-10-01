/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_recent_actions_section.h"

#include "api/api_text_entities.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/history_view_swipe_back_session.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_top_bar_widget.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "svipe/svipe_deleted_in_chat.h"
#include "svipe/svipe_message_archive.h"
#include "svipe/svipe_strings.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/shadow.h"
#include "window/window_adaptive.h"
#include "window/window_session_controller.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "styles/style_menu_icons.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

namespace Svipe {
namespace {

using namespace HistoryView;
using MessageArchive::Kind;

// The admin log's PrepareLogMessage: drop what makes no sense in a log row (views, replies, edit date,
// grouping) and date the row by the event. `out` is kept only where the bubble side should follow it:
// my own messages in a private chat stay on the right, as on Android.
[[nodiscard]] MTPMessage PrepareLogMessage(
		const MTPDmessage &data,
		TimeId newDate,
		bool keepOut) {
	using Flag = MTPDmessage::Flag;
	const auto removeFlags = Flag::f_out
		| Flag::f_post
		| Flag::f_saved_peer_id
		| Flag::f_reply_to
		| Flag::f_replies
		| Flag::f_edit_date
		| Flag::f_grouped_id
		| Flag::f_views
		| Flag::f_forwards
		| Flag::f_restriction_reason
		| Flag::f_ttl_period
		| Flag::f_factcheck
		| Flag::f_report_delivery_until_date
		| Flag::f_suggested_post
		| Flag::f_summary_from_language;
	// A synced copy of my message was uploaded by the other side, where it was incoming, so the side
	// is set from who wrote it, not from the stored flag.
	const auto flags = (data.vflags().v & ~removeFlags)
		| (keepOut ? Flag::f_out : Flag());
	return MTP_message(
		MTP_flags(flags),
		data.vid(),
		data.vfrom_id() ? *data.vfrom_id() : MTPPeer(),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		data.vpeer_id(),
		MTPPeer(), // saved_peer_id
		data.vfwd_from() ? *data.vfwd_from() : MTPMessageFwdHeader(),
		MTP_long(data.vvia_bot_id().value_or_empty()),
		MTP_long(data.vvia_business_bot_id().value_or_empty()),
		(data.vguestchat_via_from()
			? *data.vguestchat_via_from()
			: MTPPeer()),
		MTPMessageReplyHeader(),
		MTP_int(newDate),
		data.vmessage(),
		data.vmedia() ? *data.vmedia() : MTPMessageMedia(),
		data.vreply_markup() ? *data.vreply_markup() : MTPReplyMarkup(),
		(data.ventities()
			? *data.ventities()
			: MTPVector<MTPMessageEntity>()),
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTPint(), // edit_date
		MTP_string(),
		MTP_long(0), // grouped_id
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(), // ttl_period
		MTPint(), // quick_reply_shortcut_id
		MTPlong(), // effect
		MTPFactCheck(),
		MTPint(), // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(), // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
}

[[nodiscard]] mtpTypeId MediaType(const MTPDmessage &data) {
	return data.vmedia() ? data.vmedia()->type() : mtpTypeId(0);
}

// Two versions that render the same (same text, same kind of media) are not a real edit: a webpage
// preview arriving later looks like one to the edit hook.
[[nodiscard]] bool SameContent(const MTPDmessage &a, const MTPDmessage &b) {
	return (qs(a.vmessage()) == qs(b.vmessage()))
		&& (MediaType(a) == MediaType(b));
}

struct Event {
	TimeId date = 0;
	Kind kind = Kind::Deleted;
	MTPMessage message;
	std::optional<MTPMessage> previous;
};

} // namespace

void AddRecentActionsAction(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		const Ui::Menu::MenuCallback &addAction) {
	const auto history = thread->asHistory();
	if (!history || history->peer->isSelf()) {
		return;
	}
	addAction(tr::lng_manage_peer_recent_actions(tr::now), [=] {
		controller->showSection(
			std::make_shared<RecentActionsMemento>(history));
	}, &st::menuIconGroupLog);
}

RecentActionsMemento::RecentActionsMemento(not_null<History*> history)
: _history(history) {
	_list.setAroundPosition(Data::MaxMessagePosition);
}

object_ptr<Window::SectionWidget> RecentActionsMemento::createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) {
	if (column == Window::Column::Third) {
		return nullptr;
	}
	auto result = object_ptr<RecentActionsWidget>(parent, controller, _history);
	result->setInternalState(geometry, this);
	return result;
}

RecentActionsWidget::RecentActionsWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<History*> history)
: Window::SectionWidget(parent, controller, history->peer)
, WindowListDelegate(controller)
, _history(history)
, _topBar(this, controller)
, _topBarShadow(this)
, _scroll(std::make_unique<Ui::ElasticScroll>(
	this,
	controller->chatStyle()->value(lifetime(), st::historyScroll)))
, _cornerButtons(
		_scroll.get(),
		controller->chatStyle(),
		static_cast<HistoryView::CornerButtonsDelegate*>(this)) {
	controller->chatStyle()->paletteChanged(
	) | rpl::on_next([=] {
		_scroll->updateBars();
	}, _scroll->lifetime());

	Window::ChatThemeValueFromPeer(
		controller,
		history->peer
	) | rpl::on_next([=](std::shared_ptr<Ui::ChatTheme> &&theme) {
		_theme = std::move(theme);
		controller->setChatStyleTheme(_theme);
	}, lifetime());

	// Scheduled is the top bar mode with only Back and a title: no search, no call buttons.
	_topBar->setActiveChat(
		TopBarWidget::ActiveChat{
			.key = _history,
			.section = Dialogs::EntryState::Section::Scheduled,
		},
		nullptr);
	_topBar->move(0, 0);
	_topBar->resizeToWidth(width());
	_topBar->show();
	_topBar->setCustomTitle(tr::lng_manage_peer_recent_actions(tr::now));

	const auto session = &_history->session();
	const auto peerId = _history->peer->id;
	_showInChat = std::make_unique<Ui::SettingsButton>(
		this,
		TrValue(Str::ShowInChat),
		st::settingsButtonNoIcon);
	_showInChat->toggleOn(DeletedInChat::EnabledValue(session, peerId));
	_showInChat->toggledChanges(
	) | rpl::filter([=](bool enabled) {
		return enabled != DeletedInChat::Enabled(session, peerId);
	}) | rpl::on_next([=](bool enabled) {
		DeletedInChat::SetEnabled(_history, enabled);
	}, _showInChat->lifetime());
	_showInChat->show();

	_topBarShadow->raise();
	controller->adaptive().value(
	) | rpl::on_next([=] {
		updateAdaptiveLayout();
	}, lifetime());

	rebuild();
	MessageArchive::Updates(
		&_history->session()
	) | rpl::filter(
		rpl::mappers::_1 == _history->peer->id
	) | rpl::on_next([=] {
		rebuild();
		_rebuilt.fire({});
	}, lifetime());

	_scroll->setHandleTouch(false);
	_inner = _scroll->setOwnedWidget(object_ptr<ListWidget>(
		this,
		&controller->session(),
		static_cast<ListDelegate*>(this)));
	_inner->lower();
	_scroll->move(0, _topBar->height());
	_scroll->show();
	_scroll->setOverscrollBg(QColor(0, 0, 0, 0));
	_scroll->scrolls(
	) | rpl::on_next([=] {
		onScroll();
	}, lifetime());

	_inner->scrollKeyEvents(
	) | rpl::on_next([=](not_null<QKeyEvent*> e) {
		_scroll->keyPressEvent(e);
	}, lifetime());

	Window::SetupSwipeBackSection(this, _scroll.get(), _inner);
}

RecentActionsWidget::~RecentActionsWidget() {
	clearItems();
}

void RecentActionsWidget::clearItems() {
	for (const auto &item : base::take(_items)) {
		item->destroy();
	}
	_order.clear();
}

void RecentActionsWidget::rebuild() {
	clearItems();

	const auto session = &_history->session();
	const auto self = session->userPeerId();
	const auto peer = _history->peer;
	const auto privateChat = peer->isUser();

	auto events = std::vector<Event>();
	// A synced entry carries the other side's message id, so its chain is kept apart from the local
	// ones and never ends at one of my live messages that happens to share the number.
	using ChainKey = std::pair<bool, MsgId>; // (synced, message id)
	auto chains = base::flat_map<ChainKey, std::vector<MTPMessage>>();
	auto deletedById = base::flat_map<ChainKey, MTPMessage>();
	for (const auto &entry : MessageArchive::Load(session, peer->id)) {
		const auto parsed = MessageArchive::Parse(entry.message);
		if (!parsed) {
			continue;
		}
		const auto &data = parsed->c_message();
		const auto id = ChainKey(entry.id.bare == 0, MsgId(data.vid().v));
		if (entry.kind == Kind::Deleted) {
			deletedById.emplace(id, *parsed);
			events.push_back({
				.date = (entry.archivedAt
					? entry.archivedAt
					: std::max(entry.editDate, entry.date)),
				.kind = Kind::Deleted,
				.message = *parsed,
			});
		} else {
			chains[id].push_back(*parsed);
		}
	}

	// Every consecutive pair of versions is one "edited this message" event; the chain ends at the
	// live message (or at its deleted copy), so the newest edit is shown too.
	for (auto &[id, chain] : chains) {
		ranges::stable_sort(chain, ranges::less(), [](const MTPMessage &m) {
			return m.c_message().vedit_date().value_or_empty();
		});
		const auto item = id.first
			? nullptr
			: session->data().message(peer->id, id.second);
		if (item) {
			if (const auto live = MessageArchive::Parse(
					MessageArchive::LiveBytes(item))) {
				chain.push_back(*live);
			}
		} else if (const auto i = deletedById.find(id)
				; i != end(deletedById)) {
			chain.push_back(i->second);
		}
		for (auto k = 0; k + 1 < int(chain.size()); ++k) {
			const auto &prev = chain[k].c_message();
			const auto &next = chain[k + 1].c_message();
			if (SameContent(prev, next)) {
				continue;
			}
			events.push_back({
				.date = std::max(
					next.vedit_date().value_or_empty(),
					next.vdate().v),
				.kind = Kind::EditedPrior,
				.message = chain[k + 1],
				.previous = chain[k],
			});
		}
	}
	ranges::stable_sort(events, ranges::less(), &Event::date);

	auto logEntryId = WebPageId(1);
	for (const auto &event : events) {
		const auto &data = event.message.c_message();
		const auto fromId = data.vfrom_id()
			? peerFromMTP(*data.vfrom_id())
			: privateChat
			? (data.is_out() ? self : peer->id)
			: peer->id;
		const auto from = session->data().peerLoaded(fromId);
		const auto mine = (fromId == self);

		auto service = PreparedServiceText();
		if (mine) {
			service.text = { Tr(event.kind == Kind::Deleted
				? Str::YouDeletedMessage
				: Str::YouEditedMessage) };
		} else {
			const auto name = from ? from->name() : QString();
			service.text = (event.kind == Kind::Deleted
				? tr::lng_admin_log_deleted_message
				: tr::lng_admin_log_edited_message)(
					tr::now,
					lt_from,
					tr::link(name, QString()),
					tr::marked);
			if (from) {
				service.links.push_back(from->createOpenLink());
			}
		}
		_items.push_back(_history->makeMessage({
			.id = _history->nextNonHistoryEntryId(),
			.flags = MessageFlag::AdminLogEntry,
			.from = fromId,
			.date = event.date,
		}, std::move(service), nullptr));

		const auto body = _history->createItem(
			_history->nextNonHistoryEntryId(),
			PrepareLogMessage(data, event.date, privateChat && mine),
			MessageFlag::AdminLogEntry);
		if (event.previous) {
			const auto &prev = event.previous->c_message();
			auto original = TextWithEntities{
				qs(prev.vmessage()),
				Api::EntitiesFromMTP(
					session,
					prev.ventities().value_or_empty()),
			};
			if (original.text.isEmpty()) {
				original = { tr::lng_admin_log_empty_text(tr::now) };
			}
			body->addLogEntryOriginal(
				logEntryId++,
				tr::lng_admin_log_previous_message(tr::now),
				original);
		}
		_items.push_back(body);
	}
	for (auto i = 0; i != int(_items.size()); ++i) {
		_order.emplace(_items[i], i);
	}
}

Data::MessagesSlice RecentActionsWidget::slice() const {
	auto result = Data::MessagesSlice();
	result.ids.reserve(_items.size());
	for (const auto &item : _items) {
		result.ids.push_back(item->fullId());
	}
	result.skippedBefore = 0;
	result.skippedAfter = 0;
	result.fullCount = int(_items.size());
	if (!result.ids.empty()) {
		result.nearestToAround = result.ids.back();
	}
	return result;
}

Dialogs::RowDescriptor RecentActionsWidget::activeChat() const {
	return {
		_history,
		FullMsgId(_history->peer->id, ShowAtUnreadMsgId)
	};
}

QPixmap RecentActionsWidget::grabForShowAnimation(
		const Window::SectionSlideParams &params) {
	_topBar->updateControlsVisibility();
	if (params.withTopBarShadow) _topBarShadow->hide();
	auto result = Ui::GrabWidget(this);
	if (params.withTopBarShadow) _topBarShadow->show();
	return result;
}

bool RecentActionsWidget::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	if (const auto other = dynamic_cast<RecentActionsMemento*>(
			memento.get())) {
		if (other->getHistory() == _history) {
			_inner->restoreState(other->list());
			return true;
		}
	}
	return false;
}

std::shared_ptr<Window::SectionMemento> RecentActionsWidget::createMemento() {
	auto result = std::make_shared<RecentActionsMemento>(_history);
	_inner->saveState(result->list());
	return result;
}

void RecentActionsWidget::setInternalState(
		const QRect &geometry,
		not_null<RecentActionsMemento*> memento) {
	setGeometry(geometry);
	Ui::SendPendingMoveResizeEvents(this);
	_inner->restoreState(memento->list());
}

bool RecentActionsWidget::floatPlayerHandleWheelEvent(QEvent *e) {
	return _scroll->viewportEvent(e);
}

QRect RecentActionsWidget::floatPlayerAvailableRect() {
	return mapToGlobal(_scroll->geometry());
}

void RecentActionsWidget::resizeEvent(QResizeEvent *e) {
	if (!width() || !height()) {
		return;
	}
	recountChatWidth();
	updateControlsGeometry();
}

void RecentActionsWidget::recountChatWidth() {
	const auto layout = (width() < st::adaptiveChatWideWidth)
		? Window::Adaptive::ChatLayout::Normal
		: Window::Adaptive::ChatLayout::Wide;
	controller()->adaptive().setChatLayout(layout);
}

void RecentActionsWidget::updateControlsGeometry() {
	const auto contentWidth = width();
	const auto newScrollTop = _scroll->isHidden()
		? std::nullopt
		: base::make_optional(_scroll->scrollTop() + takeTopDelta());
	_topBar->resizeToWidth(contentWidth);
	_topBarShadow->resize(contentWidth, st::lineWidth);

	_showInChat->resizeToWidth(contentWidth);
	_showInChat->move(0, _topBar->height());
	const auto top = _topBar->height() + _showInChat->height();
	const auto scrollSize = QSize(contentWidth, height() - top);
	if (_scroll->size() != scrollSize) {
		_skipScrollEvent = true;
		_scroll->resize(scrollSize);
		_inner->resizeToWidth(scrollSize.width(), _scroll->height());
		_skipScrollEvent = false;
	}
	_scroll->move(0, top);
	if (!_scroll->isHidden()) {
		if (newScrollTop) {
			_scroll->scrollToY(*newScrollTop);
		}
		updateInnerVisibleArea();
	}
	_cornerButtons.updatePositions();
}

void RecentActionsWidget::updateAdaptiveLayout() {
	_topBarShadow->moveToLeft(
		controller()->adaptive().isOneColumn() ? 0 : st::lineWidth,
		_topBar->height() + (_showInChat ? _showInChat->height() : 0));
}

void RecentActionsWidget::paintEvent(QPaintEvent *e) {
	if (animatingShow()) {
		SectionWidget::paintEvent(e);
		return;
	} else if (controller()->contentOverlapped(this, e)) {
		return;
	}
	const auto aboveHeight = _topBar->height() + _showInChat->height();
	const auto bg = e->rect().intersected(
		QRect(0, aboveHeight, width(), height() - aboveHeight));
	SectionWidget::PaintBackground(controller(), _theme.get(), this, bg);
}

void RecentActionsWidget::onScroll() {
	if (_skipScrollEvent) {
		return;
	}
	updateInnerVisibleArea();
}

void RecentActionsWidget::updateInnerVisibleArea() {
	const auto scrollTop = _scroll->scrollTop();
	_inner->setVisibleTopBottom(scrollTop, scrollTop + _scroll->height());
	_cornerButtons.updateJumpDownVisibility();
}

void RecentActionsWidget::showAnimatedHook(
		const Window::SectionSlideParams &params) {
	_topBar->setAnimatingMode(true);
	if (params.withTopBarShadow) {
		_topBarShadow->show();
	}
}

void RecentActionsWidget::showFinishedHook() {
	_topBar->setAnimatingMode(false);
	_inner->showFinished();
}

void RecentActionsWidget::doSetInnerFocus() {
	_inner->setFocus();
}

void RecentActionsWidget::checkActivation() {
	_inner->checkActivation();
}

Context RecentActionsWidget::listContext() {
	return Context::Pinned;
}

bool RecentActionsWidget::listScrollTo(int top, bool syntetic) {
	top = std::clamp(top, 0, _scroll->scrollTopMax());
	if (_scroll->scrollTop() == top) {
		updateInnerVisibleArea();
		return false;
	}
	_scroll->scrollToY(top);
	return true;
}

void RecentActionsWidget::listCancelRequest() {
	controller()->showBackFromStack();
}

void RecentActionsWidget::listDeleteRequest() {
}

void RecentActionsWidget::listTryProcessKeyInput(not_null<QKeyEvent*> e) {
}

rpl::producer<Data::MessagesSlice> RecentActionsWidget::listSource(
		Data::MessagePosition aroundId,
		int limitBefore,
		int limitAfter) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		_rebuilt.events()
	) | rpl::map([=] {
		return slice();
	});
}

bool RecentActionsWidget::listAllowsMultiSelect() {
	return false;
}

bool RecentActionsWidget::listIsItemGoodForSelection(
		not_null<HistoryItem*> item) {
	return false;
}

bool RecentActionsWidget::listIsLessInOrder(
		not_null<HistoryItem*> first,
		not_null<HistoryItem*> second) {
	const auto a = _order.find(first);
	const auto b = _order.find(second);
	if (a == end(_order) || b == end(_order)) {
		return first->position() < second->position();
	}
	return a->second < b->second;
}

void RecentActionsWidget::listSelectionChanged(SelectedItems &&items) {
}

void RecentActionsWidget::listMarkReadTill(not_null<HistoryItem*> item) {
}

void RecentActionsWidget::listMarkContentsRead(
	const base::flat_set<not_null<HistoryItem*>> &items) {
}

MessagesBarData RecentActionsWidget::listMessagesBar(
		const std::vector<not_null<Element*>> &elements,
		bool markLastAsRead) {
	return {};
}

void RecentActionsWidget::listContentRefreshed() {
}

void RecentActionsWidget::listUpdateDateLink(
	ClickHandlerPtr &link,
	not_null<Element*> view) {
}

bool RecentActionsWidget::listElementHideReply(
		not_null<const Element*> view) {
	return true;
}

bool RecentActionsWidget::listElementShownUnread(
		not_null<const Element*> view) {
	return false;
}

bool RecentActionsWidget::listIsGoodForAroundPosition(
		not_null<const Element*> view) {
	return true;
}

void RecentActionsWidget::listSendBotCommand(
	const QString &command,
	const FullMsgId &context) {
}

void RecentActionsWidget::listSearch(
	const QString &query,
	const FullMsgId &context) {
}

void RecentActionsWidget::listHandleViaClick(not_null<UserData*> bot) {
}

not_null<Ui::ChatTheme*> RecentActionsWidget::listChatTheme() {
	return _theme.get();
}

CopyRestrictionType RecentActionsWidget::listCopyRestrictionType(
		HistoryItem *item) {
	return CopyRestrictionTypeFor(_history->peer, item);
}

CopyRestrictionType RecentActionsWidget::listCopyMediaRestrictionType(
		not_null<HistoryItem*> item) {
	return CopyMediaRestrictionTypeFor(_history->peer, item);
}

CopyRestrictionType RecentActionsWidget::listSelectRestrictionType() {
	return SelectRestrictionTypeFor(_history->peer);
}

auto RecentActionsWidget::listAllowedReactionsValue()
-> rpl::producer<Data::AllowedReactions> {
	return Data::PeerAllowedReactionsValue(_history->peer);
}

void RecentActionsWidget::listShowPremiumToast(
	not_null<DocumentData*> document) {
}

void RecentActionsWidget::listOpenPhoto(
		not_null<PhotoData*> photo,
		FullMsgId context) {
	controller()->openPhoto(photo, { .id = context });
}

void RecentActionsWidget::listOpenDocument(
		not_null<DocumentData*> document,
		FullMsgId context,
		bool showInMediaView) {
	controller()->openDocument(document, showInMediaView, { .id = context });
}

void RecentActionsWidget::listPaintEmpty(
		Painter &p,
		const Ui::ChatPaintContext &context) {
	if (!_items.empty()) {
		return;
	}
	// The same pill a service message is drawn in, so the empty state sits on the chat background
	// like the rest of the log.
	const auto text = Tr(Str::RecentActionsEmpty);
	const auto &font = st::msgServiceFont;
	const auto padding = st::msgServicePadding;
	const auto textWidth = std::min(
		font->width(text),
		_inner->width() - 4 * padding.left());
	const auto pill = QRect(
		(_inner->width() - textWidth) / 2 - padding.left(),
		_scroll->height() / 3,
		textWidth + padding.left() + padding.right(),
		font->height + padding.top() + padding.bottom());
	const auto radius = pill.height() / 2;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(context.st->msgServiceBg());
	p.drawRoundedRect(pill, radius, radius);
	p.setPen(context.st->msgServiceFg());
	p.setFont(font);
	p.drawText(pill, text, style::al_center);
}

QString RecentActionsWidget::listElementAuthorRank(
		not_null<const Element*> view) {
	return {};
}

bool RecentActionsWidget::listElementHideTopicButton(
		not_null<const Element*> view) {
	return true;
}

History *RecentActionsWidget::listTranslateHistory() {
	return nullptr;
}

void RecentActionsWidget::listAddTranslatedItems(
	not_null<TranslateTracker*> tracker) {
}

Ui::ElasticScroll *RecentActionsWidget::listScrollArea() const {
	return _scroll.get();
}

bool RecentActionsWidget::listThanosEffectEnabled() const {
	return false;
}

void RecentActionsWidget::cornerButtonsShowAtPosition(
		Data::MessagePosition position) {
	_inner->showAtPosition(position, {});
}

Data::Thread *RecentActionsWidget::cornerButtonsThread() {
	return _history;
}

FullMsgId RecentActionsWidget::cornerButtonsCurrentId() {
	return {};
}

bool RecentActionsWidget::cornerButtonsIgnoreVisibility() {
	return animatingShow();
}

std::optional<bool> RecentActionsWidget::cornerButtonsDownShown() {
	const auto top = _scroll->scrollTop() + st::historyToDownShownAfter;
	return (top < _scroll->scrollTopMax());
}

bool RecentActionsWidget::cornerButtonsUnreadMayBeShown() {
	return false;
}

bool RecentActionsWidget::cornerButtonsHas(CornerButtonType type) {
	return (type == CornerButtonType::Down);
}

} // namespace Svipe
