#!/bin/bash
# Svipe Desktop for Windows: CI build -> signed update package -> the site.
#
#   svipe-tools/release_win.sh                   fetch the newest successful Windows build, sign, verify
#   svipe-tools/release_win.sh --run <id>        ...a given run of .github/workflows/svipe-win.yml
#   svipe-tools/release_win.sh --publish dev     also upload to dev.svipe.uz/desktop
#   svipe-tools/release_win.sh --publish prod    ...or to svipe.uz/desktop (the owner's call, every time)
#
# MSVC cannot run on the Mac, so the Windows build happens on GitHub's runners: the workflow builds
# Svipe.exe, Updater.exe and Packer.exe, makes svipe-desktop-win64-setup.exe (first installs) and packs
# td-update-win-x64-<version> WITHOUT a signature. The release key never leaves this Mac: here the Mac
# Packer embeds the Ed25519 signature into that package and runs the client's own verification over
# it. Installed apps then update the way the Mac app does: <site>/dl/desktop/current6, key "win64".
#
# The feed carries every platform, so publishing merges "win64" into the feed the site serves now
# (the "armac" entry stays as it is) instead of writing a fresh one.
#
# Publishing mirrors release_mac.sh: <file>.new, hash checked ON the server, the old file kept as
# .bak-<version>, mv into place, .env backed up, LAVHA_DESKTOP_WIN_VERSION/_SIZE set, the app
# container recreated, then everything verified from outside.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPO=ShahzodAbdinazarov/SvipeDesktop
FILE=svipe-desktop-win64-setup.exe
KEYS="$HOME/.svipe-desktop-update-keys"
KEY_ID=svr-2026a
PACKER="$ROOT/out/Release/Packer"
SSH="ssh -i $HOME/.ssh/lavha_deploy -o ConnectTimeout=15 -o ConnectionAttempts=5 root@169.58.191.228"
SCP="scp -i $HOME/.ssh/lavha_deploy -o ConnectTimeout=15 -o ConnectionAttempts=5"

die() { echo "release_win: $*" >&2; exit 1; }

RUN=""; TARGET=""
while [ $# -gt 0 ]; do
  case "$1" in
    --run) RUN="${2:-}"; shift ;;
    --publish) TARGET="${2:-}"; shift ;;
    *) die "unknown argument $1" ;;
  esac
  shift
done

[ -x "$PACKER" ] || die "no $PACKER — build the Mac app first (release_mac.sh --build)"
[ -f "$KEYS/$KEY_ID.pem" ] || die "no release key in $KEYS"

APP_VERSION=$(grep '^AppVersion ' "$ROOT/Telegram/build/version" | awk '{print $2}')
VERSION=$(head -1 "$ROOT/Telegram/build/svipe_version")

if [ -z "$RUN" ]; then
  RUN=$(gh run list -R "$REPO" --workflow svipe-win.yml --branch dev --status success -L 1 \
    --json databaseId -q '.[0].databaseId')
  [ -n "$RUN" ] || die "no successful Windows build"
fi
SHA_BUILT=$(gh run view "$RUN" -R "$REPO" --json headSha -q .headSha)
echo "run $RUN, commit ${SHA_BUILT:0:10}"

DIR="$ROOT/out/win/$RUN"
if [ ! -f "$DIR/version.txt" ]; then
  rm -rf "$DIR"
  gh run download "$RUN" -R "$REPO" -n svipe-desktop-win64 -D "$DIR"
fi
[ "$(sed -n 1p "$DIR/version.txt" | tr -d '\r')" = "$APP_VERSION" ] \
  || die "the build is $(sed -n 1p "$DIR/version.txt") but Telegram/build/version says $APP_VERSION"
[ "$(sed -n 2p "$DIR/version.txt" | tr -d '\r')" = "$VERSION" ] \
  || die "the build is $(sed -n 2p "$DIR/version.txt") but build/svipe_version says $VERSION"
[ -f "$DIR/$FILE" ] || die "the artifact has no $FILE"

# The signature, embedded and checked by the client's own verification code (Packer refuses to write
# a package a client would reject).
UPDATE="td-update-win-x64-$APP_VERSION"
[ -f "$DIR/$UPDATE.unsigned" ] || die "the artifact has no $UPDATE.unsigned"
rm -f "$DIR/$UPDATE"
(cd "$DIR" && "$PACKER" -unsigned "$UPDATE.unsigned" -keys-loc "$KEYS" \
  -local-key "$KEYS/$KEY_ID.pem" -local-key-id "$KEY_ID")
[ -f "$DIR/$UPDATE" ] || die "Packer did not write $UPDATE"

SIZE=$(stat -f %z "$DIR/$FILE")
SHA=$(shasum -a 256 "$DIR/$FILE" | cut -d' ' -f1)
echo "update: $DIR/$UPDATE ($(stat -f %z "$DIR/$UPDATE") bytes)"
echo "installer: $DIR/$FILE"
echo "version $VERSION ($APP_VERSION), $SIZE bytes, sha256 $SHA"

[ -n "$TARGET" ] || exit 0
case "$TARGET" in
  dev)  WWW=/var/www/lavha; HOME_DIR=/home/main/lavha-dev; COMPOSE_ENV="TAG=dev"; SITE=https://dev.svipe.uz ;;
  prod) WWW=/var/www/svipe; HOME_DIR=/home/main/svipe-prod; COMPOSE_ENV=""; SITE=https://svipe.uz ;;
  *) die "--publish takes dev or prod" ;;
esac

# The feed as the site serves it now, with this platform's entry replaced.
NOW=$(curl -fsS -A "svipe-release" "$SITE/dl/desktop/current6?probe=$RANDOM$RANDOM" 2>/dev/null || echo '{}')
python3 - "$NOW" "$APP_VERSION" > "$DIR/current6" <<'PY'
import json, sys
feed = json.loads(sys.argv[1] or "{}")
feed["win64"] = {"stable": {"released": int(sys.argv[2]), "link": "/td-update-win-x64-{version}"}}
print(json.dumps(feed, separators=(",", ":")))
PY
echo "feed: $(cat "$DIR/current6")"

$SSH "mkdir -p $WWW/desktop"
$SCP "$DIR/$UPDATE" "root@169.58.191.228:$WWW/desktop/$UPDATE.new"
$SCP "$DIR/current6" "root@169.58.191.228:$WWW/desktop/current6.new"
$SCP "$DIR/$FILE" "root@169.58.191.228:$WWW/$FILE.new"
$SSH bash -s <<EOF
set -euo pipefail
cd $WWW
[ "\$(sha256sum $FILE.new | cut -d' ' -f1)" = "$SHA" ] || { echo "hash mismatch after upload"; exit 1; }
OLD=\$(grep '^LAVHA_DESKTOP_WIN_VERSION=' $HOME_DIR/.env | cut -d= -f2 || true)
[ -f $FILE ] && cp --update=none $FILE $FILE.bak-\${OLD:-unknown}
mv $FILE.new $FILE
# The package lands before the feed that points at it.
mv desktop/$UPDATE.new desktop/$UPDATE
[ -f desktop/current6 ] && cp desktop/current6 desktop/current6.bak-win-\${OLD:-unknown}
mv desktop/current6.new desktop/current6
cd $HOME_DIR
cp .env .env.bak-desktop-win-\$(date +%Y%m%d%H%M%S)
sed -i '/^LAVHA_DESKTOP_WIN_VERSION=/d;/^LAVHA_DESKTOP_WIN_SIZE=/d' .env
printf 'LAVHA_DESKTOP_WIN_VERSION=%s\nLAVHA_DESKTOP_WIN_SIZE=%s\n' "$VERSION" "$SIZE" >> .env
$COMPOSE_ENV docker compose up -d --force-recreate app >/dev/null 2>&1
EOF

# From outside, as a visitor: the served bytes and the advertised version must be what we built.
for i in $(seq 1 30); do
  curl -fsS -A "svipe-release" "$SITE/desktop?lang=en&os=win" 2>/dev/null | grep -q "$FILE" && break
  sleep 3
done
curl -fsS -A "svipe-release" "$SITE/desktop?lang=en&os=win" | grep -q "$FILE" \
  || die "$SITE/desktop does not offer $FILE"
GOT=$(curl -fsSL -A "svipe-release" "$SITE/dl/$FILE?probe=$RANDOM" | shasum -a 256 | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || die "served installer hash $GOT != $SHA"
FEED=$(curl -fsS -A "svipe-release" "$SITE/dl/desktop/current6?probe=$RANDOM")
echo "$FEED" | python3 -c "import json,sys; f=json.load(sys.stdin); assert f['win64']['stable']['released']==$APP_VERSION, f" \
  || die "feed says $FEED"
curl -fsSI -A "svipe-release" "$SITE/dl/desktop/$UPDATE" >/dev/null || die "$UPDATE is not served"
echo "published to $SITE/desktop — served bytes verified"
