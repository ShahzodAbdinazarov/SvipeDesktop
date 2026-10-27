/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A bot's tab in mode "web" (svipe.uz/bots/profile-tabs#web; Android: SvipeBotTabWebCell): the bot's page,
opened as a mini app inside the tab and as tall as the tab's page. Telegram signs the viewer into its
initData as for any mini app; svipeInitData adds where the tab is (the profile's owner or the chat).

The page scrolls with the profile until the tabs are pinned to the top, then it scrolls itself, as on
Android: the wheel over it moves the profile while the profile can still move.
*/
#pragma once

#include "ui/rp_widget.h"

namespace Main {
class Session;
} // namespace Main

namespace Svipe::WebTab {

struct Args {
	uint64 botId = 0;
	QString botUsername;
	QString url;
	PeerId peer;
	QString chatType;
};

class View final : public Ui::RpWidget {
public:
	View(QWidget *parent, not_null<Main::Session*> session, Args args);
	~View();

	void setVisibleRegion(int top, int bottom);
	void setTopOverlay(int height);
	void setActive(bool active);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	struct Private;

	void load();
	void open(not_null<UserData*> bot);
	void createHost();
	void place();
	void handle(const QString &command, const QJsonObject &arguments);
	void postEvent(const QString &event, const QJsonObject &data = {});
	[[nodiscard]] bool profileFullyScrolled() const;
	[[nodiscard]] bool covered(QRect global) const;
	[[nodiscard]] int pageHeight() const;

	const not_null<Main::Session*> _session;
	const Args _args;
	std::unique_ptr<Private> _private;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	int _overlay = 0;
	bool _active = true;

};

} // namespace Svipe::WebTab
