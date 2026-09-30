/*
Svipe Desktop — Svipe additions to Telegram Desktop.

The version people see. AppVersion (core/version.h) keeps Telegram's numbering because it is also the
tdata storage format version and what the update feed compares; this one is Svipe's own.
*/
#pragma once

#ifndef SVIPE_VERSION_STR
#define SVIPE_VERSION_STR "0.0.0"
#endif // SVIPE_VERSION_STR

namespace Svipe {

inline constexpr auto VersionStr = SVIPE_VERSION_STR;

} // namespace Svipe
