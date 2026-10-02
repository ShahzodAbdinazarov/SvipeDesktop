/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_old_identity.h"

#include "boxes/peer_list_box.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "info/info_controller.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "svipe/svipe_number_history.h"
#include "svipe/svipe_number_sync.h"
#include "svipe/svipe_strings.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"

namespace Svipe::OldIdentity {
namespace {

using namespace NumberHistory;

// Far above any id Telegram has issued; never loaded, so it always draws as a deleted account.
constexpr auto kUnknownUserId = uint64(1) << 47;

struct Item {
	UserData *user = nullptr; // set only when Telegram shows us the account
	uint64 userId = 0;
	QString phone; // where the account went (old profiles) or the number left behind (old numbers)
};

[[nodiscard]] UserData *Visible(not_null<Main::Session*> session, uint64 userId) {
	if (!userId) {
		return nullptr;
	}
	const auto user = session->data().userLoaded(UserId(userId));
	return (user && !user->isInaccessible()) ? user : nullptr;
}

// The number an account is on today: what Telegram shows now, failing that the last we wrote down.
[[nodiscard]] QString CurrentNumberOf(not_null<UserData*> user) {
	const auto phone = Normalize(user->phone());
	if (!phone.isEmpty()) {
		return phone;
	}
	const auto numbers = NumbersOfAccount(peerToUser(user->id).bare);
	return numbers.empty() ? QString() : numbers.back().phone;
}

// SvipeOldIdentity.oldProfiles: what this device watched happen on the person's number, plus the ids
// the server sends without a number attached. An id is only a place to look.
[[nodiscard]] std::vector<Item> OldProfiles(not_null<UserData*> user) {
	const auto session = &user->session();
	const auto self = peerToUser(user->id).bare;
	auto ids = std::vector<uint64>();
	const auto add = [&](uint64 id) {
		if (id && id != self && !ranges::contains(ids, id)) {
			ids.push_back(id);
		}
	};
	if (const auto phone = CurrentNumberOf(user); !phone.isEmpty()) {
		for (const auto &seen : AccountsOnNumber(phone)) {
			add(seen.userId);
		}
	}
	for (const auto id : NumberHistory::OldProfiles(self)) {
		add(id);
	}
	auto result = std::vector<Item>();
	for (const auto id : ids) {
		const auto visible = Visible(session, id);
		result.push_back({
			.user = visible,
			.userId = id,
			// Where they are now, only if Telegram itself hands it to us.
			.phone = visible ? Normalize(visible->phone()) : QString(),
		});
	}
	return result;
}

// SvipeOldIdentity.oldNumbers: every number but the current one, each with whoever holds it now.
[[nodiscard]] std::vector<Item> OldNumbers(not_null<UserData*> user) {
	const auto session = &user->session();
	const auto self = peerToUser(user->id).bare;
	const auto numbers = NumbersOfAccount(self);
	auto result = std::vector<Item>();
	if (numbers.size() < 2) {
		return result; // one number is not a history
	}
	for (auto i = 0; i + 1 < int(numbers.size()); ++i) {
		const auto &phone = numbers[i].phone;
		auto holder = std::optional<Account>();
		for (const auto &seen : AccountsOnNumber(phone)) {
			if (seen.userId && seen.userId != self
				&& (!holder || seen.lastSeen > holder->lastSeen)) {
				holder = seen; // whoever we saw on it most recently holds it now
			}
		}
		auto visible = holder ? Visible(session, holder->userId) : nullptr;
		if (!visible) {
			// Nobody we have seen holds it: Telegram will say who does, asked once ever per number.
			const auto resolved = Resolved(phone);
			if (resolved > 0) {
				visible = Visible(session, uint64(resolved));
			} else if (resolved == -1) {
				Resolve(session, phone);
			}
		}
		result.push_back({
			.user = visible,
			.userId = holder ? holder->userId : 0,
			.phone = phone,
		});
	}
	return result;
}

[[nodiscard]] QString FormatPhone(const QString &digits) {
	return digits.isEmpty() ? QString() : (u"+"_q + digits);
}

class Row final : public PeerListRow {
public:
	Row(not_null<PeerData*> peer, uint64 id, bool openable)
	: PeerListRow(peer, id)
	, _openable(openable) {
	}

	QString generateName() override {
		return _openable ? PeerListRow::generateName() : tr::lng_deleted(tr::now);
	}
	QString generateShortName() override {
		return _openable
			? PeerListRow::generateShortName()
			: tr::lng_deleted(tr::now);
	}
	[[nodiscard]] bool openable() const {
		return _openable;
	}

private:
	const bool _openable = false;

};

class Controller final : public PeerListController {
public:
	Controller(
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		bool numbers)
	: _window(window)
	, _user(user)
	, _numbers(numbers) {
	}

	Main::Session &session() const override {
		return _user->session();
	}
	void prepare() override {
		refresh();
		Updates() | rpl::on_next([=] {
			refresh();
		}, _lifetime);
	}
	void rowClicked(not_null<PeerListRow*> row) override {
		if (static_cast<Row*>(row.get())->openable()) {
			_window->showPeerInfo(row->peer());
		}
	}

private:
	void refresh() {
		while (delegate()->peerListFullRowsCount()) {
			delegate()->peerListRemoveRow(delegate()->peerListRowAt(0));
		}
		auto index = uint64(0);
		const auto items = _numbers ? OldNumbers(_user) : OldProfiles(_user);
		for (const auto &item : items) {
			// An account we cannot see is an unloaded user of that id (or of an id no account has,
			// when we never learnt it), which Telegram draws as a deleted account.
			const auto peer = item.user
				? not_null<PeerData*>(item.user)
				: not_null<PeerData*>(_user->owner().user(
					UserId(item.userId ? item.userId : kUnknownUserId)));
			auto row = std::make_unique<Row>(peer, ++index, item.user != nullptr);
			row->setCustomStatus(FormatPhone(item.phone));
			delegate()->peerListAppendRow(std::move(row));
		}
		delegate()->peerListRefreshRows();
	}

	const not_null<Window::SessionController*> _window;
	const not_null<UserData*> _user;
	const bool _numbers = false;
	rpl::lifetime _lifetime;

};

class Content final : public Ui::RpWidget {
public:
	Content(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		bool numbers)
	: RpWidget(parent)
	, _controller(window, user, numbers)
	, _list(this, &_controller) {
		_controller.setStyleOverrides(&st::infoCommonGroupsList);
		_delegate.setContent(_list.data());
		_controller.setDelegate(&_delegate);
		_list->moveToLeft(0, st::infoCommonGroupsMargin.top());
		widthValue() | rpl::on_next([=](int width) {
			_list->resizeToWidth(width);
		}, lifetime());
		_list->heightValue() | rpl::on_next([=](int height) {
			resize(width(), st::infoCommonGroupsMargin.top()
				+ height
				+ st::infoCommonGroupsMargin.bottom());
		}, lifetime());
	}

protected:
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		setChildVisibleTopBottom(_list.data(), visibleTop, visibleBottom);
	}

private:
	Controller _controller;
	PeerListContentDelegateSimple _delegate;
	object_ptr<PeerListContent> _list;

};

class TabAdapter final : public Info::Profile::MediaTabContent {
public:
	TabAdapter(
		Info::Profile::MediaTabContext context,
		not_null<UserData*> user,
		bool numbers)
	: _content(
		context.parent,
		context.controller->parentController(),
		user,
		numbers)
	, _user(user)
	, _numbers(numbers) {
	}

	not_null<Ui::RpWidget*> widget() override {
		return _content.data();
	}
	Info::Profile::TabTopBarBindings topBarBindings() override {
		return {
			.title = TrValue(_numbers
				? Str::OldNumbersTab
				: Str::OldProfilesTab) | rpl::map([](QString text) {
					return TextWithEntities{ text };
				}),
		};
	}
	void resizeToWidth(int newWidth) override {
		_content->resizeToWidth(newWidth);
	}
	void setVisibleRegion(int top, int bottom) override {
		_content->setVisibleTopBottom(top, bottom);
	}

private:
	object_ptr<Content> _content;
	const not_null<UserData*> _user;
	const bool _numbers = false;

};

[[nodiscard]] rpl::producer<bool> ShownValue(
		not_null<UserData*> user,
		bool numbers) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Updates()
	) | rpl::map([=] {
		return !(numbers ? OldNumbers(user) : OldProfiles(user)).empty();
	});
}

[[nodiscard]] Info::Profile::MediaTabDescriptor MakeTab(
		not_null<UserData*> user,
		bool numbers) {
	return {
		.id = numbers ? u"svipe_old_numbers"_q : u"svipe_old_profiles"_q,
		.title = TrValue(numbers
			? Str::OldNumbersTab
			: Str::OldProfilesTab) | rpl::map([](QString text) {
				return TextWithEntities{ text };
			}),
		.shown = ShownValue(user, numbers),
		.factory = [=](Info::Profile::MediaTabContext context) {
			return std::make_unique<TabAdapter>(std::move(context), user, numbers);
		},
	};
}

} // namespace

Info::Profile::MediaTabDescriptor MakeOldProfilesTab(not_null<UserData*> user) {
	// The pooled past often holds the change this device never saw: ask when the profile opens.
	NumberSync::SyncProfile(user);
	return MakeTab(user, false);
}

Info::Profile::MediaTabDescriptor MakeOldNumbersTab(not_null<UserData*> user) {
	return MakeTab(user, true);
}

} // namespace Svipe::OldIdentity
