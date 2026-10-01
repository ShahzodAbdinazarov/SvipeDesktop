/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_profile_images.h"

#include "apiwrap.h"
#include "api/api_peer_photo.h"
#include "base/weak_ptr.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "info/info_controller.h"
#include "info/media/info_media_buttons.h"
#include "info/media/info_media_common.h"
#include "info/profile/info_profile_icon.h"
#include "info/media/info_media_list_section.h"
#include "info/media/info_media_widget.h"
#include "info/media/info_media_list_widget.h"
#include "main/main_session.h"
#include "overview/overview_layout.h"
#include "svipe/svipe_avatar_archive.h"
#include "svipe/svipe_strings.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "ui/ui_utility.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"

#include <QtGui/QImageReader>

namespace Svipe::ProfileImages {
namespace {

using namespace Info;
using namespace Info::Media;

struct Entry {
	PhotoId id = 0;
	TimeId date = 0;
	bool deleted = false;
};

// Live photos as Telegram lists them, then the kept ones it no longer does; newest set first, stable
// (SvipeProfileImages.mergeRefs).
[[nodiscard]] std::vector<Entry> CollectEntries(not_null<UserData*> user) {
	const auto userId = peerToUser(user->id);
	auto &owner = user->owner();
	auto result = std::vector<Entry>();
	for (const auto id : AvatarArchive::Live(user).ids) {
		const auto photo = owner.photo(id);
		if (!photo->isNull()) {
			result.push_back({ .id = id, .date = photo->date() });
		}
	}
	for (const auto &photo : AvatarArchive::Deleted(user)) {
		result.push_back({ .id = photo.id, .date = photo.date, .deleted = true });
	}
	ranges::stable_sort(result, [](const Entry &a, const Entry &b) {
		return (a.date != b.date) ? (a.date > b.date) : (a.id > b.id);
	});
	return result;
}

[[nodiscard]] rpl::producer<int> CountValue(not_null<UserData*> user) {
	return rpl::single(
		peerToUser(user->id)
	) | rpl::then(
		AvatarArchive::Updates()
	) | rpl::filter(
		rpl::mappers::_1 == peerToUser(user->id)
	) | rpl::map([=] {
		return int(CollectEntries(user).size());
	});
}

// A deleted photo is no longer anywhere in Telegram's data, so it comes back as a local PhotoData
// with our copy as its large image: the grid cell and the media viewer then show it like any other.
[[nodiscard]] not_null<PhotoData*> PhotoFor(
		not_null<UserData*> user,
		const Entry &entry) {
	auto &owner = user->owner();
	const auto existing = owner.photo(entry.id);
	if (!entry.deleted || !existing->isNull()) {
		return existing;
	}
	auto reader = QImageReader(AvatarArchive::FilePath(
		peerToUser(user->id),
		entry.id));
	reader.setAutoTransform(true);
	const auto image = reader.read();
	if (image.isNull()) {
		return existing;
	}
	auto large = ImageWithLocation{ .preloaded = image };
	return owner.photo(
		entry.id,
		0, // access_hash
		QByteArray(), // file_reference
		entry.date,
		0, // dc
		false, // has stickers
		QByteArray(), // inline thumbnail
		ImageWithLocation(),
		ImageWithLocation(),
		large,
		ImageWithLocation(),
		ImageWithLocation(),
		crl::time(0));
}

// Bound to the window, not to a profile: the classic layout shows the grid in a box that may outlive
// the profile it was opened from.
class SubController final : public AbstractController {
public:
	SubController(
		not_null<Window::SessionController*> window,
		not_null<UserData*> user)
	: AbstractController(window)
	, _user(user)
	, _key(user) {
	}

	Key key() const override {
		return _key;
	}
	PeerData *migrated() const override {
		return nullptr;
	}
	Section section() const override {
		return Section(Section::MediaType::Photo);
	}
	[[nodiscard]] not_null<UserData*> user() const {
		return _user;
	}

private:
	const not_null<UserData*> _user;
	const Key _key;

};

class Provider final
	: public ListProvider
	, private ListSectionDelegate
	, public base::has_weak_ptr {
public:
	explicit Provider(not_null<UserData*> user)
	: _user(user)
	, _history(user->owner().history(user)) {
		style::PaletteChanged(
		) | rpl::on_next([=] {
			for (auto &layout : _layouts) {
				layout.second.item->invalidateCache();
			}
		}, _lifetime);
		AvatarArchive::Updates(
		) | rpl::filter(
			rpl::mappers::_1 == peerToUser(user->id)
		) | rpl::on_next([=] {
			refreshViewer();
		}, _lifetime);
	}

	Type type() override {
		return Type::PhotoVideo;
	}
	bool hasSelectRestriction() override {
		return true;
	}
	rpl::producer<bool> hasSelectRestrictionChanges() override {
		return rpl::never<bool>();
	}
	bool isPossiblyMyItem(not_null<const HistoryItem*> item) override {
		return true;
	}
	std::optional<int> fullCount() override {
		return int(_entries.size());
	}
	void restart() override {
		refreshViewer();
	}
	void checkPreload(
			QSize viewport,
			not_null<BaseLayout*> topLayout,
			not_null<BaseLayout*> bottomLayout,
			bool preloadTop,
			bool preloadBottom) override {
	}
	void refreshViewer() override {
		_entries = CollectEntries(_user);
		_refreshed.fire({});
	}
	rpl::producer<> refreshed() override {
		return _refreshed.events();
	}
	void setSearchQuery(QString query) override {
	}
	void jumpToMessage(MsgId, Fn<void(FullMsgId)>) override {
	}

	std::vector<ListSection> fillSections(
			not_null<Overview::Layout::Delegate*> delegate) override {
		for (auto &layout : _layouts) {
			layout.second.stale = true;
		}
		auto result = std::vector<ListSection>();
		auto section = ListSection(Type::PhotoVideo, sectionDelegate());
		for (const auto &entry : _entries) {
			if (const auto layout = getLayout(entry, delegate)) {
				if (!section.addItem(layout)) {
					section.finishSection();
					result.push_back(std::move(section));
					section = ListSection(Type::PhotoVideo, sectionDelegate());
					section.addItem(layout);
				}
			}
		}
		if (!section.empty()) {
			section.finishSection();
			result.push_back(std::move(section));
		}
		for (auto i = begin(_layouts); i != end(_layouts);) {
			if (i->second.stale) {
				_layoutRemoved.fire(i->second.item.get());
				_items.remove(i->first);
				i = _layouts.erase(i);
			} else {
				++i;
			}
		}
		return result;
	}
	rpl::producer<not_null<BaseLayout*>> layoutRemoved() override {
		return _layoutRemoved.events();
	}
	BaseLayout *lookupLayout(const HistoryItem *item) override {
		for (const auto &[id, layout] : _layouts) {
			if (layout.item->getItem() == item) {
				return layout.item.get();
			}
		}
		return nullptr;
	}
	bool isMyItem(not_null<const HistoryItem*> item) override {
		return ranges::any_of(_items, [&](const auto &pair) {
			return pair.second.get() == item.get();
		});
	}
	bool isAfter(
			not_null<const HistoryItem*> a,
			not_null<const HistoryItem*> b) override {
		return indexOf(a) > indexOf(b);
	}
	ListItemSelectionData computeSelectionData(
			not_null<const HistoryItem*> item,
			TextSelection selection) override {
		return ListItemSelectionData(selection);
	}
	void applyDragSelection(
			ListSelectedMap &selected,
			not_null<const HistoryItem*> fromItem,
			bool skipFrom,
			not_null<const HistoryItem*> tillItem,
			bool skipTill) override {
	}
	bool allowSaveFileAs(
			not_null<const HistoryItem*> item,
			not_null<DocumentData*> document) override {
		return false;
	}
	QString showInFolderPath(
			not_null<const HistoryItem*> item,
			not_null<DocumentData*> document) override {
		return QString();
	}
	int64 scrollTopStatePosition(not_null<HistoryItem*> item) override {
		return indexOf(item);
	}
	HistoryItem *scrollTopStateItem(ListScrollTopState state) override {
		return state.item;
	}
	void saveState(
			not_null<Info::Media::Memento*> memento,
			ListScrollTopState scrollState) override {
	}
	void restoreState(
			not_null<Info::Media::Memento*> memento,
			Fn<void(ListScrollTopState)> restoreScrollState) override {
	}

private:
	bool sectionHasFloatingHeader() override {
		return false;
	}
	QString sectionTitle(not_null<const BaseLayout*> item) override {
		return QString();
	}
	bool sectionItemBelongsHere(
			not_null<const BaseLayout*> item,
			not_null<const BaseLayout*> previous) override {
		return true;
	}

	[[nodiscard]] int indexOf(not_null<const HistoryItem*> item) const {
		for (auto i = 0; i != int(_entries.size()); ++i) {
			const auto j = _items.find(_entries[i].id);
			if (j != end(_items) && j->second.get() == item.get()) {
				return i;
			}
		}
		return -1;
	}

	[[nodiscard]] BaseLayout *getLayout(
			const Entry &entry,
			not_null<Overview::Layout::Delegate*> delegate) {
		auto i = _layouts.find(entry.id);
		if (i == end(_layouts)) {
			const auto photo = PhotoFor(_user, entry);
			if (photo->isNull()) {
				return nullptr;
			}
			// A fake item, like a story's: Local + FakeHistoryItem keeps it out of the chat and out
			// of every server request; it exists only so the grid cell and the viewer can host it.
			const auto item = _history->makeMessage({
				.id = _user->owner().nextLocalMessageId(),
				.flags = (MessageFlag::Local
					| MessageFlag::HasFromId
					| MessageFlag::FakeHistoryItem),
				.from = _user->id,
				.date = entry.date,
			}, photo, TextWithEntities());
			_items[entry.id] = std::shared_ptr<HistoryItem>(
				item.get(),
				HistoryItem::Destroyer());
			auto layout = std::make_unique<Overview::Layout::Photo>(
				delegate,
				item,
				photo,
				Overview::Layout::MediaOptions{
					.svipeDeleted = entry.deleted,
				});
			layout->initDimensions();
			i = _layouts.emplace(entry.id, std::move(layout)).first;
		}
		i->second.stale = false;
		return i->second.item.get();
	}

	const not_null<UserData*> _user;
	const not_null<History*> _history;
	std::vector<Entry> _entries;
	base::flat_map<PhotoId, std::shared_ptr<HistoryItem>> _items;
	std::unordered_map<PhotoId, CachedItem> _layouts;
	rpl::event_stream<not_null<BaseLayout*>> _layoutRemoved;
	rpl::event_stream<> _refreshed;
	rpl::lifetime _lifetime;

};

class TabAdapter final : public Info::Profile::MediaTabContent {
public:
	TabAdapter(
		Info::Profile::MediaTabContext context,
		not_null<UserData*> user)
	: _user(user)
	, _subController(context.controller->parentController(), user)
	, _host(context.parent) {
		const auto host = _host.data();
		_list = Ui::CreateChild<ListWidget>(host, &_subController);
		_list->show();
		_list->heightValue(
		) | rpl::on_next([this](int newHeight) {
			_host->resize(_host->width(), newHeight);
		}, host->lifetime());
		_list->scrollToRequests(
		) | rpl::on_next([this, scrollTo = context.scrollToRequest](
				int top) {
			if (scrollTo && _list->isVisible() && _topOverlay > 0) {
				scrollTo(std::max(top, 0), -1);
			}
		}, host->lifetime());
	}

	not_null<Ui::RpWidget*> widget() override {
		return _host.data();
	}
	void resizeToWidth(int newWidth) override {
		if (_host->width() != newWidth) {
			_list->resizeToWidth(std::max(
				newWidth - st::infoMediaTabsRightSkip,
				1));
		}
		_host->resize(newWidth, _list->height());
	}
	Info::Profile::TabTopBarBindings topBarBindings() override {
		return {
			.title = TrValue(Str::ProfileImages) | rpl::map([](QString text) {
				return TextWithEntities{ text };
			}),
			.subtitle = CountValue(_user) | rpl::map([](int count) {
				return TextWithEntities{ ProfileImagesCount(count) };
			}),
			.selectedItems = _list->selectedListValue(),
			.selectionAction = crl::guard(
				base::make_weak(_list),
				[list = _list](SelectionAction action) {
					list->selectionAction(action);
				}),
		};
	}
	void deactivated() override {
		_list->selectionAction(SelectionAction::Clear);
	}
	void setVisibleRegion(int top, int bottom) override {
		_list->setExternalViewportHeight(bottom - top);
		_list->setVisibleTopBottom(top, bottom);
	}
	void setTopOverlay(int height) override {
		_topOverlay = height;
		_list->setTopOverlayHeight(height);
	}

private:
	const not_null<UserData*> _user;
	SubController _subController;
	object_ptr<Ui::RpWidget> _host;
	ListWidget *_list = nullptr;
	int _topOverlay = 0;

};

void ShowBox(
		not_null<Window::SessionController*> window,
		not_null<UserData*> user) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TrValue(Str::ProfileImages));
		box->setWidth(st::boxWideWidth);
		const auto controller = new SubController(window, user);
		const auto list = box->addRow(
			object_ptr<ListWidget>(box, controller),
			QMargins());
		// The list outlives its members' teardown only through its own lifetime.
		list->lifetime().add([=] { delete controller; });
		const auto update = [=] {
			const auto top = box->scrollTop() - list->y();
			list->setVisibleTopBottom(top, top + box->scrollHeight());
		};
		box->scrolls() | rpl::on_next(update, list->lifetime());
		list->heightValue() | rpl::on_next([=](int) {
			update();
		}, list->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace

void AddClassicButton(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		Ui::MultiSlideTracker &tracker,
		const style::icon &icon) {
	if (user->isBot()) {
		return;
	}
	user->session().api().peerPhoto().requestUserPhotos(user, {});
	const auto wrap = Info::Media::AddCountedButton(
		parent,
		CountValue(user),
		[](int count) { return ProfileImagesButton(count); },
		tracker);
	object_ptr<Info::Profile::FloatingIcon>(
		wrap->entity(),
		icon,
		st::infoSharedMediaButtonIconPosition);
	wrap->entity()->addClickHandler([=] {
		ShowBox(window, user);
	});
}

Info::Profile::MediaTabDescriptor MakeTabDescriptor(not_null<UserData*> user) {
	// Opening the profile loads the photo list, as the Android profile does: that is both what the
	// tab shows and what captures new photos.
	user->session().api().peerPhoto().requestUserPhotos(user, {});
	return {
		.id = u"svipe_profile_images"_q,
		.title = TrValue(Str::ProfileImages) | rpl::map([](QString text) {
			return TextWithEntities{ text };
		}),
		.shown = CountValue(user) | rpl::map(rpl::mappers::_1 > 0),
		.factory = [=](Info::Profile::MediaTabContext context) {
			return std::make_unique<TabAdapter>(std::move(context), user);
		},
	};
}

std::unique_ptr<ListProvider> MakeProvider(
		not_null<AbstractController*> controller) {
	if (const auto sub = dynamic_cast<SubController*>(controller.get())) {
		return std::make_unique<Provider>(sub->user());
	}
	return nullptr;
}

} // namespace Svipe::ProfileImages
