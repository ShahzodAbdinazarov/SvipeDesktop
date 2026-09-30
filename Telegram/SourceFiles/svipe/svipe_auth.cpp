/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_auth.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "svipe/svipe_api.h"
#include "svipe/svipe_config.h"
#include "svipe/svipe_storage.h"

#include <QtCore/QUrl>

namespace Svipe::Auth {
namespace {

constexpr auto kResolveTimeout = crl::time(8000);
constexpr auto kWebViewTimeout = crl::time(12000);
constexpr auto kExpiryMarginMs = qint64(60000);

const auto kToken = u"svipe_access_token"_q;
const auto kRefresh = u"svipe_refresh_token"_q;
const auto kExpires = u"svipe_token_expires"_q;

base::flat_map<uint64, std::vector<Fn<void(QString)>>> &InFlight() {
	static auto result = base::flat_map<uint64, std::vector<Fn<void(QString)>>>();
	return result;
}

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

QString StoredToken(not_null<Main::Session*> session) {
	const auto token = Storage::Get(session, kToken).toString();
	const auto expires = qint64(Storage::Get(session, kExpires).toDouble());
	return (!token.isEmpty() && NowMs() < expires - kExpiryMarginMs)
		? token
		: QString();
}

void StoreTokens(not_null<Main::Session*> session, const QJsonObject &result) {
	Storage::Set(session, kToken, result.value(u"access_token"_q).toString());
	// A refresh answer may omit the refresh token; that must not wipe the one we hold.
	const auto refresh = result.value(u"refresh_token"_q).toString();
	if (!refresh.isEmpty()) {
		Storage::Set(session, kRefresh, refresh);
	}
	const auto expiresIn = result.value(u"expires_in"_q).toInt(3600);
	Storage::Set(session, kExpires, double(NowMs() + qint64(expiresIn) * 1000));
}

[[nodiscard]] bool IsOk(const QJsonObject &result) {
	return result.value(u"status"_q).toString() == u"ok"_q;
}

void Finish(not_null<Main::Session*> session, const QString &token) {
	auto callbacks = std::move(InFlight()[session->userId().bare]);
	InFlight().remove(session->userId().bare);
	for (const auto &callback : callbacks) {
		callback(token);
	}
}

void Refresh(not_null<Main::Session*> session, Fn<void(QString)> done) {
	const auto refresh = Storage::Get(session, kRefresh).toString();
	if (refresh.isEmpty()) {
		done(QString());
		return;
	}
	auto body = QJsonObject();
	body.insert(u"refresh_token"_q, refresh);
	Api::Post(u"/v1/auth/refresh"_q, body, QString(), crl::guard(session, [=](
			QJsonObject result,
			int code) {
		if (IsOk(result)) {
			StoreTokens(session, result);
			done(result.value(u"access_token"_q).toString());
			return;
		} else if (code == 401) {
			// Revoked or expired on the server — do not keep riding a dead token.
			Storage::Remove(session, kRefresh);
		}
		done(QString());
	}));
}

void ResolveBot(
		not_null<Main::Session*> session,
		Fn<void(UserData*)> done) {
	const auto username = AuthBotUsername();
	if (const auto peer = session->data().peerByUsername(username)) {
		if (const auto user = peer->asUser()) {
			done(user);
			return;
		}
	}
	const auto answered = std::make_shared<bool>(false);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTP_string()
	)).done(crl::guard(session, [=](const MTPcontacts_ResolvedPeer &result) {
		if (std::exchange(*answered, true)) {
			return;
		}
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(peerFromMTP(data.vpeer()));
		done(peer ? peer->asUser() : nullptr);
	})).fail(crl::guard(session, [=] {
		if (!std::exchange(*answered, true)) {
			done(nullptr);
		}
	})).send();
	base::call_delayed(kResolveTimeout, session, [=] {
		if (!std::exchange(*answered, true)) {
			done(nullptr);
		}
	});
}

void WebAppAuth(not_null<Main::Session*> session, Fn<void(QString)> done) {
	ResolveBot(session, [=](UserData *bot) {
		if (!bot) {
			done(QString());
			return;
		}
		using Flag = MTPmessages_RequestWebView::Flag;
		const auto answered = std::make_shared<bool>(false);
		session->api().request(MTPmessages_RequestWebView(
			MTP_flags(Flag::f_url | Flag::f_from_bot_menu),
			bot->input(),
			bot->inputUser(),
			MTP_string(WebAppUrl()),
			MTPstring(), // start_param
			MTPDataJSON(), // theme_params
			MTP_string("tdesktop"),
			MTPInputReplyTo(),
			MTPInputPeer() // send_as
		)).done(crl::guard(session, [=](const MTPWebViewResult &result) {
			if (std::exchange(*answered, true)) {
				return;
			}
			const auto initData = result.match([&](
					const MTPDwebViewResultUrl &data) {
				return ExtractInitData(qs(data.vurl()));
			});
			if (initData.isEmpty()) {
				done(QString());
				return;
			}
			auto body = QJsonObject();
			body.insert(u"init_data"_q, initData);
			Api::Post(
				u"/v1/auth/telegram/webapp"_q,
				body,
				QString(),
				crl::guard(session, [=](QJsonObject result, int code) {
					if (IsOk(result)) {
						StoreTokens(session, result);
						done(result.value(u"access_token"_q).toString());
					} else {
						done(QString());
					}
				}));
		})).fail(crl::guard(session, [=] {
			if (!std::exchange(*answered, true)) {
				done(QString());
			}
		})).send();
		base::call_delayed(kWebViewTimeout, session, [=] {
			if (!std::exchange(*answered, true)) {
				done(QString());
			}
		});
	});
}

} // namespace

QString ExtractInitData(const QString &webViewUrl) {
	const auto hash = webViewUrl.indexOf('#');
	if (hash < 0 || hash == webViewUrl.size() - 1) {
		return QString();
	}
	const auto prefix = u"tgWebAppData="_q;
	for (const auto &param : webViewUrl.mid(hash + 1).split('&')) {
		if (param.startsWith(prefix)) {
			const auto value = param.mid(prefix.size());
			// Decoded exactly once: the backend's parse_qsl performs the second decode, matching how
			// Telegram computes the hash over fully decoded values.
			return value.isEmpty()
				? QString()
				: QUrl::fromPercentEncoding(value.toUtf8());
		}
	}
	return QString();
}

void EnsureToken(not_null<Main::Session*> session, Fn<void(QString)> done) {
	if (const auto token = StoredToken(session); !token.isEmpty()) {
		done(token);
		return;
	}
	auto &waiting = InFlight()[session->userId().bare];
	waiting.push_back(std::move(done));
	if (waiting.size() > 1) {
		return; // Someone is already asking; this caller rides along.
	}
	Refresh(session, crl::guard(session, [=](QString token) {
		if (!token.isEmpty()) {
			Finish(session, token);
			return;
		}
		WebAppAuth(session, crl::guard(session, [=](QString token) {
			Finish(session, token);
		}));
	}));
}

} // namespace Svipe::Auth
