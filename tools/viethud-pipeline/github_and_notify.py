#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
github_and_notify.py — publish build/speedmap/ to the VietHUD GitHub repo and
notify Telegram, but ONLY when the data actually changed.

Steps:
  1. Ensure a local clone of the repo exists at repo/ (clone if missing, else pull).
  2. Copy the freshly packed .bin files into repo/<speedmap>/.
  3. Compute SHA-256 of each core .bin and write manifest.txt
     (line 1: `version <YYYY.MM.DD.HHMM>`, then `<name> <bytes> <sha256>` — the
     exact format the firmware DataUpdater parses; raster/sounds excluded).
  4. If manifest.txt is byte-identical to the repo's current one, STOP (no commit,
     no push, no Telegram) — keeps daily runs from spamming when nothing changed.
  5. git add/commit/push to the configured branch.
  6. Send a Telegram Markdown summary.

Auth: set GITHUB_TOKEN in .env (a fine-grained PAT with contents:write on the
repo). SSH deploy keys work too — then leave GITHUB_TOKEN empty and point
VIETHUD_GIT_REMOTE at the git@ URL.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import requests

from vhpaths import (LOG, SPEEDMAP_OUT, REPO_DIR, STATE_DIR, GIT_REMOTE, GIT_BRANCH,
                     GIT_SPEEDMAP_SUBDIR, TELEGRAM_TOKEN, TELEGRAM_CHAT_ID,
                     RAW_MANIFEST_URL)

# Files that go into manifest.txt (the OTA set). Raster + sounds excluded, exactly
# like the current hand-made manifest.
MANIFEST_FILES = ["metadata.bin", "index.bin", "tiles.bin", "cameras.bin",
                  "signs.bin", "names.bin", "seg_names.bin"]


def _run(cmd, cwd=None, check=True):
    LOG.info("$ %s", " ".join(cmd))
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.stdout.strip():
        LOG.info(r.stdout.strip())
    if r.returncode != 0:
        LOG.error(r.stderr.strip())
        if check:
            raise RuntimeError("command failed: %s" % " ".join(cmd))
    return r


def _auth_remote():
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if token and GIT_REMOTE.startswith("https://"):
        return GIT_REMOTE.replace("https://", f"https://x-access-token:{token}@", 1)
    return GIT_REMOTE


def ensure_repo():
    repo = Path(REPO_DIR)
    if (repo / ".git").exists():
        _run(["git", "remote", "set-url", "origin", _auth_remote()], cwd=repo, check=False)
        _run(["git", "fetch", "origin", GIT_BRANCH], cwd=repo, check=False)
        _run(["git", "checkout", GIT_BRANCH], cwd=repo, check=False)
        _run(["git", "reset", "--hard", f"origin/{GIT_BRANCH}"], cwd=repo, check=False)
    else:
        repo.parent.mkdir(parents=True, exist_ok=True)
        if repo.exists():
            shutil.rmtree(repo)
        _run(["git", "clone", "--branch", GIT_BRANCH, "--depth", "1", _auth_remote(), str(repo)])
    # identity for commits (local to this repo)
    _run(["git", "config", "user.email", os.environ.get("GIT_AUTHOR_EMAIL", "pipeline@viethud.local")], cwd=repo, check=False)
    _run(["git", "config", "user.name", os.environ.get("GIT_AUTHOR_NAME", "VietHUD Pipeline")], cwd=repo, check=False)
    return repo


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def build_manifest(speedmap_dir, version):
    lines = [f"version {version}"]
    for name in MANIFEST_FILES:
        p = speedmap_dir / name
        if not p.exists():
            LOG.warning("manifest: %s missing in build output — skipping", name)
            continue
        lines.append(f"{name} {p.stat().st_size} {sha256_file(p)}")
    return "\n".join(lines) + "\n"


def telegram(summary, manifest_changed=True):
    if not TELEGRAM_TOKEN or not TELEGRAM_CHAT_ID:
        LOG.warning("Telegram not configured (.env) — skipping notification")
        return
    text = (
        "🚗 *VietHUD Map Data Update Thành Công*\n"
        f"⏱ Phiên bản: `{summary['map_version']}`\n"
        f"📍 Camera: *{summary['cameras']:,}* điểm\n"
        f"🚦 Đèn giao thông (Type 6): *{summary['traffic_lights']:,}* điểm\n"
        f"🛑 Tổng biển báo: *{summary['signs']:,}*\n"
        f"🛣 Tuyến/segment: *{summary['segments']:,}*  ·  Tên phố V2: *{summary['names']:,}*\n"
        f"🧩 Tiles: *{summary['tiles']:,}*\n"
        f"🔗 Manifest OTA: {RAW_MANIFEST_URL}"
    )
    try:
        r = requests.post(
            f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage",
            json={"chat_id": TELEGRAM_CHAT_ID, "text": text,
                  "parse_mode": "Markdown", "disable_web_page_preview": True},
            timeout=30)
        if r.ok:
            LOG.info("Telegram notified")
        else:
            LOG.error("Telegram failed: %s %s", r.status_code, r.text[:200])
    except Exception as e:
        LOG.error("Telegram error: %s", e)


def main():
    speedmap_dir = Path(SPEEDMAP_OUT)
    summary_path = speedmap_dir.parent / "last_pack_summary.json"
    if not summary_path.exists():
        LOG.error("no last_pack_summary.json — run synthesize_and_pack.py first")
        return 1
    summary = json.loads(summary_path.read_text())
    version = summary["map_version"]

    repo = ensure_repo()
    repo_speedmap = repo / GIT_SPEEDMAP_SUBDIR
    repo_speedmap.mkdir(parents=True, exist_ok=True)

    new_manifest = build_manifest(speedmap_dir, version)
    old_manifest = ""
    old_mf_path = repo_speedmap / "manifest.txt"
    if old_mf_path.exists():
        old_manifest = old_mf_path.read_text()

    # Change detection ignores the version line (timestamp changes every run);
    # compare only the file/size/sha body.
    def body(m):
        return "\n".join(l for l in m.splitlines() if not l.startswith("version "))
    if body(new_manifest) == body(old_manifest):
        LOG.info("manifest body unchanged — nothing to publish (no commit/push/notify)")
        return 0

    # Copy the fresh bins into the repo, then write manifest.
    for name in MANIFEST_FILES:
        src = speedmap_dir / name
        if src.exists():
            shutil.copy2(src, repo_speedmap / name)
    old_mf_path.write_text(new_manifest, encoding="utf-8")

    _run(["git", "add", "-A", GIT_SPEEDMAP_SUBDIR], cwd=repo)
    msg = (f"data: update speedmap {version} "
           f"(signs {summary['signs']}, lights {summary['traffic_lights']}, "
           f"cameras {summary['cameras']}, names {summary['names']})")
    commit = _run(["git", "commit", "-m", msg], cwd=repo, check=False)
    if "nothing to commit" in (commit.stdout + commit.stderr).lower():
        LOG.info("git: nothing to commit (files identical) — done")
        return 0
    _run(["git", "push", "origin", GIT_BRANCH], cwd=repo)
    LOG.info("pushed %s to %s/%s", version, GIT_REMOTE, GIT_BRANCH)

    (STATE_DIR / "last_published.json").write_text(json.dumps(
        dict(version=version, at=time.strftime("%Y-%m-%d %H:%M:%S"), **summary), indent=2))
    telegram(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
