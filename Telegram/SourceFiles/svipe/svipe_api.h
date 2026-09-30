/*
Svipe Desktop — Svipe additions to Telegram Desktop.

A small JSON client for the Svipe backend (the Android app's SvipeApi.java). Every call answers on
the main thread exactly once: the parsed object (empty when the body was not JSON) and the HTTP code
(0 when the request never got an answer).
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Svipe::Api {

using Done = Fn<void(QJsonObject result, int code)>;

void Get(const QString &path, const QString &token, Done done);
void Put(
	const QString &path,
	const QJsonObject &body,
	const QString &token,
	Done done);
void Post(
	const QString &path,
	const QJsonObject &body,
	const QString &token,
	Done done);

} // namespace Svipe::Api
