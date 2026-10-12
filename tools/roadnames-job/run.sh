#!/usr/bin/env bash
# VietHUD road-name channels — weekly, OpenStreetMap only (ODbL).
# Independent of every other data job: own checkout, own folders on GitHub
# (roadnames/, roadnames_lite154/), own signed manifests. See
# tools/map_builder/publish_roadnames.py and src/net/NamesUpdater.cpp.
set -euo pipefail
JOB=/home/admin/viethud-names
REPO=$JOB/repo
PBF=$JOB/data/vietnam-latest.osm.pbf
mkdir -p "$JOB/data" "$JOB/logs"
exec >>"$JOB/logs/run_$(date +%Y%m).log" 2>&1
echo "=== $(date '+%F %T') start"

# 1. checkout: sparse + shallow — only what this job reads/writes
if [ ! -d "$REPO/.git" ]; then
    git clone -q --depth 1 --filter=blob:none --sparse git@github.com:911273/VietHUD.git "$REPO"
    git -C "$REPO" sparse-checkout set --no-cone /roadnames/ /roadnames_lite154/ \
        /tools/sign_manifest.py /tools/map_builder/build_road_names.py /tools/map_builder/publish_roadnames.py
    git -C "$REPO" config user.name "VietHUD Road Names Bot"
    git -C "$REPO" config user.email "roadnames-bot@viethud.local"
fi
git -C "$REPO" pull -q --rebase --depth 1 origin main || git -C "$REPO" pull -q --rebase origin main

# 2. OSM extract: only re-downloaded when Geofabrik has a newer one
curl -fsSL -z "$PBF" -o "$PBF.new" https://download.geofabrik.de/asia/vietnam-latest.osm.pbf && \
    { [ -s "$PBF.new" ] && mv "$PBF.new" "$PBF" || rm -f "$PBF.new"; }
ls -la "$PBF"

# 3. build, sign (key ~/.viethud), publish only if the names changed
python3 "$REPO/tools/map_builder/publish_roadnames.py" --pbf "$PBF" --repo "$REPO" --push
echo "=== $(date '+%F %T') done"
