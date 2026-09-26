#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_osm.py — download vietnam-latest.osm.pbf from Geofabrik (ODbL), skipping
the download when the server copy hasn't changed.

Change detection (no full re-download when unchanged):
  1. HTTP conditional GET with If-None-Match (ETag) + If-Modified-Since, using
     the ETag/Last-Modified we stored from the previous successful download.
     A 304 Not Modified => nothing to do.
  2. Geofabrik's companion `<file>.md5` is fetched first (tiny); if it matches
     the md5 we recorded last time, we skip too.
Atomic: writes to <file>.part then os.replace() so a partial download never
replaces a good PBF. Verifies the md5 after download.

Runs monthly from the systemd timer, but is safe to run any time.
"""
import hashlib
import json
import os
import sys
import time
from pathlib import Path

import requests

from vhpaths import DATA_DIR, LOG  # shared paths/logging

GEOFABRIK_PBF = "https://download.geofabrik.de/asia/vietnam-latest.osm.pbf"
GEOFABRIK_MD5 = GEOFABRIK_PBF + ".md5"

PBF_PATH = DATA_DIR / "osm" / "vietnam-latest.osm.pbf"
STATE_PATH = DATA_DIR / "osm" / "fetch_state.json"
CHUNK = 1 << 20  # 1 MiB


def _load_state():
    try:
        return json.loads(STATE_PATH.read_text())
    except Exception:
        return {}


def _save_state(state):
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(state, indent=2))


def _remote_md5(session):
    try:
        r = session.get(GEOFABRIK_MD5, timeout=30)
        if r.ok:
            # format: "<md5>  vietnam-latest.osm.pbf"
            return r.text.strip().split()[0].lower()
    except Exception as e:
        LOG.warning("could not fetch .md5 companion: %s", e)
    return None


def _file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(CHUNK), b""):
            h.update(blk)
    return h.hexdigest()


def main():
    PBF_PATH.parent.mkdir(parents=True, exist_ok=True)
    state = _load_state()
    session = requests.Session()
    session.headers["User-Agent"] = "VietHUD-pipeline/1.0 (ODbL OSM sync; contact operator)"

    remote_md5 = _remote_md5(session)
    if remote_md5 and remote_md5 == state.get("md5") and PBF_PATH.exists():
        if _file_md5(PBF_PATH) == remote_md5:
            LOG.info("OSM PBF unchanged (md5 %s) — skip download", remote_md5)
            return 0
        LOG.warning("recorded md5 matches remote but local file differs — re-downloading")

    headers = {}
    if PBF_PATH.exists():
        if state.get("etag"):
            headers["If-None-Match"] = state["etag"]
        if state.get("last_modified"):
            headers["If-Modified-Since"] = state["last_modified"]

    LOG.info("downloading %s", GEOFABRIK_PBF)
    t0 = time.time()
    with session.get(GEOFABRIK_PBF, headers=headers, stream=True, timeout=(30, 600)) as r:
        if r.status_code == 304:
            LOG.info("server returned 304 Not Modified — skip download")
            return 0
        r.raise_for_status()
        total = int(r.headers.get("Content-Length", 0))
        part = PBF_PATH.with_suffix(PBF_PATH.suffix + ".part")
        got = 0
        with open(part, "wb") as f:
            for chunk in r.iter_content(CHUNK):
                if chunk:
                    f.write(chunk)
                    got += len(chunk)
        if total and got != total:
            part.unlink(missing_ok=True)
            LOG.error("short download: %d/%d bytes", got, total)
            return 2

        # verify md5 if we have one
        if remote_md5:
            actual = _file_md5(part)
            if actual != remote_md5:
                part.unlink(missing_ok=True)
                LOG.error("md5 mismatch: got %s want %s", actual, remote_md5)
                return 3

        os.replace(part, PBF_PATH)
        state.update(
            etag=r.headers.get("ETag"),
            last_modified=r.headers.get("Last-Modified"),
            md5=remote_md5 or _file_md5(PBF_PATH),
            bytes=got,
            fetched_at=time.strftime("%Y-%m-%d %H:%M:%S"),
        )
        _save_state(state)
    LOG.info("downloaded %.1f MB in %.0fs -> %s", got / 1e6, time.time() - t0, PBF_PATH)
    return 0


if __name__ == "__main__":
    sys.exit(main())
