#!/usr/bin/env python3
"""Svipe Desktop's own update trust root, in tdesktop's v2 format.

tdesktop verifies every update package against a pinned root Ed25519 key and a key manifest that
root signed (Telegram/Resources/update/: root-public.pem, manifest.min.json, manifest.sig). Those
files are Telegram's: left in place, a Svipe build would trust Telegram's packages and reject ours.
This script makes the same three files for Svipe, plus the release key the packages are signed with.

    svipe-tools/update_keys.py init      generate the keys once, write Resources/update/*
    svipe-tools/update_keys.py manifest  re-issue the manifest (key rotation, expiry) with the root

The private keys live outside the repo, in ~/.svipe-desktop-update-keys (0700). Lose them and no
installed Svipe Desktop can be updated again — back that folder up.
"""
import base64
import json
import os
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

KEYS = Path.home() / ".svipe-desktop-update-keys"
RESOURCES = Path(__file__).resolve().parent.parent / "Telegram" / "Resources" / "update"
RELEASE_KEY_ID = "svr-2026a"
MANIFEST_VERSION = 1
LIFETIME = 5 * 365 * 24 * 3600


def b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def load(name: str) -> Ed25519PrivateKey:
    return serialization.load_pem_private_key((KEYS / name).read_bytes(), password=None)


def save(name: str, key: Ed25519PrivateKey) -> None:
    path = KEYS / name
    if path.exists():
        sys.exit(f"{path} already exists — refusing to overwrite a signing key")
    path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    path.chmod(0o600)


def raw_public(key: Ed25519PrivateKey) -> bytes:
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def write_manifest(root: Ed25519PrivateKey, release: Ed25519PrivateKey) -> None:
    now = int(time.time())
    manifest = {
        "format": 1,
        "manifest_version": MANIFEST_VERSION,
        "issued": now,
        "expires": now + LIFETIME,
        "keys": [{"id": RELEASE_KEY_ID, "alg": "Ed25519", "x": b64url(raw_public(release))}],
        # Stable and beta are the only lanes Svipe ships; no canary keys, so a canary package can
        # never verify.
        "channels": {"stable": [[RELEASE_KEY_ID]], "beta": [[RELEASE_KEY_ID]]},
        "revoked": [],
    }
    data = json.dumps(manifest, separators=(",", ":")).encode()
    (RESOURCES / "manifest.min.json").write_bytes(data)
    (RESOURCES / "manifest.sig").write_bytes(root.sign(data))
    (RESOURCES / "root-public.pem").write_bytes(root.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    # Packer reads the same trio through -keys-loc; keep a copy beside the private keys.
    for name in ("manifest.min.json", "manifest.sig", "root-public.pem"):
        (KEYS / name).write_bytes((RESOURCES / name).read_bytes())


def main() -> None:
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "init":
        KEYS.mkdir(mode=0o700, exist_ok=True)
        os.chmod(KEYS, 0o700)
        root, release = Ed25519PrivateKey.generate(), Ed25519PrivateKey.generate()
        save("root-private.pem", root)
        save(f"{RELEASE_KEY_ID}.pem", release)
        write_manifest(root, release)
    elif command == "manifest":
        write_manifest(load("root-private.pem"), load(f"{RELEASE_KEY_ID}.pem"))
    else:
        sys.exit(__doc__)
    print(f"keys in {KEYS}, public trio in {RESOURCES}")


if __name__ == "__main__":
    main()
