/*
Svipe Desktop — Svipe additions to Telegram Desktop.

Per-account key/value store for what Svipe keeps on the client: the backend tokens and the
message-type notification rules. The Android app keeps the same keys in SharedPreferences; here they
live in one JSON file per account under the working directory, so a Debug build (its own tdata) and
a release build never share them.
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Main {
class Session;
} // namespace Main

namespace Svipe::Storage {

[[nodiscard]] QJsonValue Get(not_null<Main::Session*> session, const QString &key);
void Set(not_null<Main::Session*> session, const QString &key, const QJsonValue &value);
void Remove(not_null<Main::Session*> session, const QString &key);
[[nodiscard]] QStringList KeysWithPrefix(
	not_null<Main::Session*> session,
	const QString &prefix);

} // namespace Svipe::Storage
