#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_pipeline.py — the daily job. Packs the current OSM PBF + custom alerts, then
publishes to GitHub + Telegram only if the data changed. Fetches the PBF first
only if it's missing (the monthly timer normally keeps it fresh).
"""
import sys
from pathlib import Path

from vhpaths import LOG, DATA_DIR
import synthesize_and_pack
import github_and_notify
import fetch_osm


def main():
    pbf = DATA_DIR / "osm" / "vietnam-latest.osm.pbf"
    if not pbf.exists():
        LOG.info("PBF missing — fetching before packing")
        rc = fetch_osm.main()
        if rc != 0 or not pbf.exists():
            LOG.error("cannot pack without a PBF (fetch rc=%s)", rc)
            return rc or 1

    rc = synthesize_and_pack.main()
    if rc != 0:
        LOG.error("pack failed (rc=%s) — not publishing", rc)
        return rc

    return github_and_notify.main()


if __name__ == "__main__":
    sys.exit(main())
