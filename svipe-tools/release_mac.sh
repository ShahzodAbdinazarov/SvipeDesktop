#!/bin/bash
# Svipe Desktop for macOS: Release build -> signed Svipe.app -> svipe-desktop-mac.dmg -> the site.
#
#   svipe-tools/release_mac.sh                  package the existing out/Release/Svipe.app
#   svipe-tools/release_mac.sh --build          configure + build Release first
#   svipe-tools/release_mac.sh --publish dev    also upload to dev.svipe.uz/desktop
#   svipe-tools/release_mac.sh --publish prod   ...or to svipe.uz/desktop (the owner's call, every time)
#
# The .dmg is for first installs. Installed apps update themselves the way Telegram Desktop does:
# they poll <site>/dl/desktop/current6 and fetch a v2 package (td-update-mac-arm-<version>) that
# Packer signs with our release key (svipe-tools/update_keys.py); tdesktop's own Updater swaps the
# bundle and relaunches. Bump the version first with Telegram/build/set_version.sh.
#
# Publishing mirrors the Android .web recipe: upload as <file>.new, check the hash ON the server,
# keep the old file as .bak-<version>, mv into place (nginx never serves half a file), back up .env,
# set LAVHA_DESKTOP_MAC_VERSION/_SIZE, recreate the app container, then verify from outside.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP="$ROOT/out/Release/Svipe.app"
DMG="$ROOT/out/Release/svipe-desktop-mac.dmg"
FILE=svipe-desktop-mac.dmg
BUNDLE_ID=uz.svipe.mac
KEYS="$HOME/.svipe-desktop-update-keys"
KEY_ID=svr-2026a
SSH="ssh -i $HOME/.ssh/lavha_deploy -o ConnectTimeout=15 -o ConnectionAttempts=5 root@169.58.191.228"
SCP="scp -i $HOME/.ssh/lavha_deploy -o ConnectTimeout=15 -o ConnectionAttempts=5"
export PATH=/opt/homebrew/bin:$PATH

die() { echo "release_mac: $*" >&2; exit 1; }

BUILD=0; TARGET=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD=1 ;;
    --publish) TARGET="${2:-}"; shift ;;
    *) die "unknown argument $1" ;;
  esac
  shift
done

if [ "$BUILD" = 1 ]; then
  APIKEYS="$HOME/StudioProjects/Lavha/apikeys.properties"
  ID=$(grep -i 'api_id\|APP_ID' "$APIKEYS" | head -1 | cut -d= -f2 | tr -d ' "')
  HASH=$(grep -i 'api_hash\|APP_HASH' "$APIKEYS" | head -1 | cut -d= -f2 | tr -d ' "')
  (cd "$ROOT/Telegram" && ./configure.sh -D TDESKTOP_API_ID="$ID" -D TDESKTOP_API_HASH="$HASH" \
    -D DESKTOP_APP_DISABLE_AUTOUPDATE=OFF -D CMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -D CMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS \
    -D CMAKE_OBJCXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS >/dev/null)
  # Xcode does not reprocess the bundle's Info.plist when only the configured template changed,
  # so a Svipe version bump shipped as the old number (1.0.12 went out as "1.0.11"). Drop it.
  rm -f "$ROOT/out/Release/Svipe.app/Contents/Info.plist"
  (cd "$ROOT/out" && xcodebuild -project Telegram.xcodeproj -scheme Telegram -configuration Release \
    -destination 'platform=macOS,arch=arm64' -jobs 10 build | grep -E "error:|BUILD (SUCCEEDED|FAILED)")
fi

[ -d "$APP" ] || die "no $APP — run with --build"

# The updater must be built in, and it must trust OUR root: with Telegram's Resources/update files a
# Svipe build would reject every Svipe package and accept Telegram's.
grep -q "^DESKTOP_APP_DISABLE_AUTOUPDATE:BOOL=OFF" "$ROOT/out/CMakeCache.txt" \
  || die "auto-update is disabled in out/CMakeCache.txt — run with --build"
cmp -s "$ROOT/Telegram/Resources/update/root-public.pem" "$KEYS/root-public.pem" \
  || die "Resources/update is not Svipe's update root (svipe-tools/update_keys.py)"
[ -x "$ROOT/out/Release/Packer" ] || die "no out/Release/Packer — run with --build"
[ -x "$APP/Contents/Frameworks/Updater" ] || die "the bundle has no Updater"
PLIST="$APP/Contents/Info.plist"
ID_BUILT=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$PLIST")
[ "$ID_BUILT" = "$BUNDLE_ID" ] || die "bundle id is $ID_BUILT, expected $BUNDLE_ID"
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$PLIST")
lipo -archs "$APP/Contents/MacOS/Svipe" | grep -q arm64 || die "no arm64 slice"

# As tdesktop's own Telegram/build/build.sh does: keep the symbols in a .dSYM next to the build (for
# crash reports), then strip the shipped binary — unstripped it is ~835 MB.
BIN="$APP/Contents/MacOS/Svipe"
if [ "$(nm "$BIN" 2>/dev/null | wc -l)" -gt 200000 ]; then
  dsymutil "$BIN" -o "$ROOT/out/Release/Svipe.app.dSYM"
  strip "$BIN"
fi

# No Developer ID yet, so the app is ad-hoc signed: a clean, consistent signature lets macOS offer
# "Open Anyway"; a broken one makes it say the app is damaged.
codesign --force --deep --sign - --preserve-metadata=entitlements,flags "$APP"
codesign --verify --deep --strict "$APP"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -quiet -volname "Svipe" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
# The update package, from the very bundle inside the dmg.
APP_VERSION=$(grep '^AppVersion ' "$ROOT/Telegram/build/version" | awk '{print $2}')
UPDATE="td-update-mac-arm-$APP_VERSION"
(cd "$ROOT/out/Release" && rm -f "$UPDATE" && ./Packer -path Svipe.app -arch arm64 -version "$APP_VERSION" \
  -channel stable -keys-loc "$KEYS" -local-key "$KEYS/$KEY_ID.pem" -local-key-id "$KEY_ID" >/dev/null)
[ -f "$ROOT/out/Release/$UPDATE" ] || die "Packer did not write $UPDATE"
printf '{"armac":{"stable":{"released":%s,"link":"/td-update-mac-arm-{version}"}}}\n' "$APP_VERSION" \
  > "$ROOT/out/Release/current6"
echo "update: $UPDATE ($(stat -f %z "$ROOT/out/Release/$UPDATE") bytes)"

SIZE=$(stat -f %z "$DMG")
SHA=$(shasum -a 256 "$DMG" | cut -d' ' -f1)
echo "dmg: $DMG"
echo "version $VERSION, $SIZE bytes, sha256 $SHA"
[ "$VERSION" = "$(head -1 "$ROOT/Telegram/build/svipe_version")" ] \
  || die "the bundle says $VERSION but build/svipe_version says $(head -1 "$ROOT/Telegram/build/svipe_version")"

[ -n "$TARGET" ] || exit 0
case "$TARGET" in
  dev)  WWW=/var/www/lavha; HOME_DIR=/home/main/lavha-dev; COMPOSE_ENV="TAG=dev"; SITE=https://dev.svipe.uz ;;
  prod) WWW=/var/www/svipe; HOME_DIR=/home/main/svipe-prod; COMPOSE_ENV=""; SITE=https://svipe.uz ;;
  *) die "--publish takes dev or prod" ;;
esac

$SSH "mkdir -p $WWW/desktop"
$SCP "$ROOT/out/Release/$UPDATE" "root@169.58.191.228:$WWW/desktop/$UPDATE.new"
$SCP "$ROOT/out/Release/current6" "root@169.58.191.228:$WWW/desktop/current6.new"
$SCP "$DMG" "root@169.58.191.228:$WWW/$FILE.new"
$SSH bash -s <<EOF
set -euo pipefail
cd $WWW
[ "\$(sha256sum $FILE.new | cut -d' ' -f1)" = "$SHA" ] || { echo "hash mismatch after upload"; exit 1; }
OLD=\$(grep '^LAVHA_DESKTOP_MAC_VERSION=' $HOME_DIR/.env | cut -d= -f2 || true)
[ -f $FILE ] && cp --update=none $FILE $FILE.bak-\${OLD:-unknown}
mv $FILE.new $FILE
# The package lands before the feed that points at it.
mv desktop/$UPDATE.new desktop/$UPDATE
[ -f desktop/current6 ] && cp desktop/current6 desktop/current6.bak-\${OLD:-unknown}
mv desktop/current6.new desktop/current6
cd $HOME_DIR
cp .env .env.bak-desktop-\$(date +%Y%m%d%H%M%S)
sed -i '/^LAVHA_DESKTOP_MAC_VERSION=/d;/^LAVHA_DESKTOP_MAC_SIZE=/d' .env
printf 'LAVHA_DESKTOP_MAC_VERSION=%s\nLAVHA_DESKTOP_MAC_SIZE=%s\n' "$VERSION" "$SIZE" >> .env
$COMPOSE_ENV docker compose up -d --force-recreate app >/dev/null 2>&1
EOF

# From outside, as a visitor: the served bytes and the advertised version must be what we built.
for i in $(seq 1 30); do
  curl -fsS -A "svipe-release" "$SITE/desktop?lang=en" 2>/dev/null | grep -q "version $VERSION" && break
  sleep 3
done
curl -fsS -A "svipe-release" "$SITE/desktop?lang=en" | grep -q "version $VERSION" \
  || die "$SITE/desktop does not show version $VERSION"
GOT=$(curl -fsSL -A "svipe-release" "$SITE/dl/$FILE" | shasum -a 256 | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || die "served dmg hash $GOT != $SHA"
FEED=$(curl -fsS -A "svipe-release" "$SITE/dl/desktop/current6")
echo "$FEED" | grep -q "\"released\":$APP_VERSION" || die "feed says $FEED"
curl -fsSI -A "svipe-release" "$SITE/dl/desktop/$UPDATE" >/dev/null || die "$UPDATE is not served"
echo "published to $SITE/desktop — served bytes verified"
