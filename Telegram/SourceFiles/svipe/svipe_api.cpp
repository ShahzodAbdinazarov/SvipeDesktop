/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_api.h"

#include "svipe/svipe_config.h"

#include <QtCore/QJsonDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Svipe::Api {
namespace {

constexpr auto kTimeoutMs = 15000;

QNetworkAccessManager &Manager() {
	static auto result = QNetworkAccessManager();
	return result;
}

QNetworkRequest Request(const QString &path, const QString &token) {
	auto result = QNetworkRequest(QUrl(BaseUrl() + path));
	result.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	if (!token.isEmpty()) {
		result.setRawHeader("Authorization", "Bearer " + token.toUtf8());
	}
	result.setTransferTimeout(kTimeoutMs);
	return result;
}

void Finish(QNetworkReply *reply, Done done) {
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto document = QJsonDocument::fromJson(reply->readAll());
		reply->deleteLater();
		if (done) {
			done(document.isObject() ? document.object() : QJsonObject(), code);
		}
	});
}

QByteArray Serialize(const QJsonObject &body) {
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

} // namespace

void Get(const QString &path, const QString &token, Done done) {
	Finish(Manager().get(Request(path, token)), std::move(done));
}

void Put(
		const QString &path,
		const QJsonObject &body,
		const QString &token,
		Done done) {
	Finish(
		Manager().put(Request(path, token), Serialize(body)),
		std::move(done));
}

void Post(
		const QString &path,
		const QJsonObject &body,
		const QString &token,
		Done done) {
	Finish(
		Manager().post(Request(path, token), Serialize(body)),
		std::move(done));
}

void PutBytes(
		const QString &url,
		const QByteArray &bytes,
		const QString &contentType,
		Fn<void(int code)> done) {
	auto request = QNetworkRequest(QUrl(url.startsWith('/') ? (BaseUrl() + url) : url));
	request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
	request.setTransferTimeout(4 * kTimeoutMs);
	const auto reply = Manager().put(request, bytes);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		reply->deleteLater();
		if (done) {
			done(code);
		}
	});
}

void GetBytes(const QString &url, Fn<void(QByteArray bytes, int code)> done) {
	auto request = QNetworkRequest(QUrl(url.startsWith('/') ? (BaseUrl() + url) : url));
	request.setTransferTimeout(4 * kTimeoutMs);
	const auto reply = Manager().get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		auto bytes = reply->readAll();
		reply->deleteLater();
		if (done) {
			done(std::move(bytes), code);
		}
	});
}

void Delete(const QString &path, const QString &token, Done done) {
	Finish(Manager().deleteResource(Request(path, token)), std::move(done));
}

} // namespace Svipe::Api
