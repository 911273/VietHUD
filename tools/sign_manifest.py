#!/usr/bin/env python3
"""Sign a VietHUD data manifest (manifest.txt -> manifest.txt.sig).

The firmware (src/update/DataInstaller.cpp) refuses to install any data set
whose manifest.txt is not signed by this key — both for the device's own
online update (Mode A) and for data pushed from a phone (Phone Update Bridge).
So EVERY publish of speedmap/manifest.txt must be followed by this script,
otherwise devices report "Ban cap nhat chua duoc ky".

Signature: ECDSA P-256 / SHA-256 over the exact bytes of manifest.txt, DER
encoded, base64 on one line. Private key: $VIETHUD_SIGNING_KEY or
~/.viethud/manifest_signing_key.pem (never commit it). The matching public key
is compiled into the firmware (kManifestPubKeyPem).

Usage:  python tools/sign_manifest.py speedmap/manifest.txt [more manifests...]
        python tools/sign_manifest.py --verify speedmap/manifest.txt
"""
import base64
import os
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


def key_path() -> Path:
    env = os.environ.get("VIETHUD_SIGNING_KEY")
    return Path(env) if env else Path.home() / ".viethud" / "manifest_signing_key.pem"


def load_key():
    p = key_path()
    if not p.exists():
        sys.exit(f"signing key not found: {p} (set VIETHUD_SIGNING_KEY)")
    return serialization.load_pem_private_key(p.read_bytes(), None)


def sign(manifest: Path, key) -> Path:
    data = manifest.read_bytes()
    sig = key.sign(data, ec.ECDSA(hashes.SHA256()))
    out = manifest.with_name(manifest.name + ".sig")
    out.write_text(base64.b64encode(sig).decode() + "\n", encoding="ascii", newline="\n")
    return out


def verify(manifest: Path, key) -> bool:
    sig_path = manifest.with_name(manifest.name + ".sig")
    try:
        sig = base64.b64decode(sig_path.read_text().strip())
        key.public_key().verify(sig, manifest.read_bytes(), ec.ECDSA(hashes.SHA256()))
        return True
    except (FileNotFoundError, InvalidSignature, ValueError):
        return False


def main(argv):
    verify_only = "--verify" in argv
    files = [Path(a) for a in argv if a != "--verify"]
    if not files:
        sys.exit(__doc__)
    key = load_key()
    ok = True
    for m in files:
        if verify_only:
            good = verify(m, key)
            ok &= good
            print(f"{m}: {'OK' if good else 'BAD/MISSING signature'}")
        else:
            print(f"signed {m} -> {sign(m, key)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
