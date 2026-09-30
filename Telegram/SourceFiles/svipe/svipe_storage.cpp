/*
Svipe Desktop — Svipe additions to Telegram Desktop.
*/
#include "svipe/svipe_storage.h"

#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

namespace Svipe::Storage {
namespace {

struct Store {
	QJsonObject values;
	bool loaded = false;
};

base::flat_map<uint64, Store> &Stores() {
	static auto result = base::flat_map<uint64, Store>();
	return result;
}

QString PathFor(uint64 userId) {
	return cWorkingDir() + u"tdata/svipe/"_q + QString::number(userId) + u".json"_q;
}

Store &StoreFor(not_null<Main::Session*> session) {
	const auto userId = session->userId().bare;
	auto &store = Stores()[userId];
	if (!store.loaded) {
		store.loaded = true;
		auto file = QFile(PathFor(userId));
		if (file.open(QIODevice::ReadOnly)) {
			const auto document = QJsonDocument::fromJson(file.readAll());
			if (document.isObject()) {
				store.values = document.object();
			}
		}
	}
	return store;
}

void Save(not_null<Main::Session*> session, const Store &store) {
	const auto path = PathFor(session->userId().bare);
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(store.values).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

} // namespace

QJsonValue Get(not_null<Main::Session*> session, const QString &key) {
	return StoreFor(session).values.value(key);
}

void Set(
		not_null<Main::Session*> session,
		const QString &key,
		const QJsonValue &value) {
	auto &store = StoreFor(session);
	if (store.values.value(key) == value) {
		return;
	}
	store.values.insert(key, value);
	Save(session, store);
}

void Remove(not_null<Main::Session*> session, const QString &key) {
	auto &store = StoreFor(session);
	if (!store.values.contains(key)) {
		return;
	}
	store.values.remove(key);
	Save(session, store);
}

QStringList KeysWithPrefix(
		not_null<Main::Session*> session,
		const QString &prefix) {
	auto result = QStringList();
	const auto &values = StoreFor(session).values;
	for (auto i = values.begin(); i != values.end(); ++i) {
		if (i.key().startsWith(prefix)) {
			result.push_back(i.key());
		}
	}
	return result;
}

} // namespace Svipe::Storage
