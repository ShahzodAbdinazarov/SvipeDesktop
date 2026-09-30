#!/bin/bash
# Svipe Desktop for macOS: Release build -> signed Svipe.app -> svipe-desktop-mac.dmg -> the site.
#
#   svipe-tools/release_mac.sh                  package the existing out/Release/Svipe.app
#   svipe-tools/release_mac.sh --build          configure + build Release first
#   svipe-tools/release_mac.sh --publish dev    also upload to dev.svipe.uz/desktop
#   svipe-tools/release_mac.sh --publish prod   ...or to svipe.uz/desktop (the owner's call, every time)
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
SSH="ssh -i $HOME/.ssh/lavha_deploy -o ConnectTimeout=15 root@169.58.191.228"
SCP="scp -i $HOME/.ssh/lavha_deploy -o ConnectTimeout=15"
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
  KEYS="$HOME/StudioProjects/Lavha/apikeys.properties"
  ID=$(grep -i 'api_id\|APP_ID' "$KEYS" | head -1 | cut -d= -f2 | tr -d ' "')
  HASH=$(grep -i 'api_hash\|APP_HASH' "$KEYS" | head -1 | cut -d= -f2 | tr -d ' "')
  (cd "$ROOT/Telegram" && ./configure.sh -D TDESKTOP_API_ID="$ID" -D TDESKTOP_API_HASH="$HASH" \
    -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON -D CMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -D CMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS \
    -D CMAKE_OBJCXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS >/dev/null)
  (cd "$ROOT/out" && xcodebuild -project Telegram.xcodeproj -scheme Telegram -configuration Release \
    -destination 'platform=macOS,arch=arm64' -jobs 10 build | grep -E "error:|BUILD (SUCCEEDED|FAILED)")
fi

[ -d "$APP" ] || die "no $APP — run with --build"

# tdesktop's updater downloads OFFICIAL Telegram builds with Telegram's keys: left on, the first
# update would replace Svipe with Telegram. Check the configured cache, not the source.
grep -q "^DESKTOP_APP_DISABLE_AUTOUPDATE:BOOL=ON" "$ROOT/out/CMakeCache.txt" \
  || die "auto-update is not disabled in out/CMakeCache.txt"
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
SIZE=$(stat -f %z "$DMG")
SHA=$(shasum -a 256 "$DMG" | cut -d' ' -f1)
echo "dmg: $DMG"
echo "version $VERSION, $SIZE bytes, sha256 $SHA"

[ -n "$TARGET" ] || exit 0
case "$TARGET" in
  dev)  WWW=/var/www/lavha; HOME_DIR=/home/main/lavha-dev; COMPOSE_ENV="TAG=dev"; SITE=https://dev.svipe.uz ;;
  prod) WWW=/var/www/svipe; HOME_DIR=/home/main/svipe-prod; COMPOSE_ENV=""; SITE=https://svipe.uz ;;
  *) die "--publish takes dev or prod" ;;
esac

$SCP "$DMG" "root@169.58.191.228:$WWW/$FILE.new"
$SSH bash -s <<EOF
set -euo pipefail
cd $WWW
[ "\$(sha256sum $FILE.new | cut -d' ' -f1)" = "$SHA" ] || { echo "hash mismatch after upload"; exit 1; }
OLD=\$(grep '^LAVHA_DESKTOP_MAC_VERSION=' $HOME_DIR/.env | cut -d= -f2 || true)
[ -f $FILE ] && cp -n $FILE $FILE.bak-\${OLD:-unknown}
mv $FILE.new $FILE
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
echo "published to $SITE/desktop — served bytes verified"
