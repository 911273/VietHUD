#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
synthesize_and_pack.py — turn the OSM PBF (+ the operator's own surveyed alerts)
into the VietHUD firmware binary set under build/speedmap/.

Sources (both ODbL / operator-owned — NO third-party proprietary data):
  * OpenStreetMap vietnam-latest.osm.pbf (roads, names, maxspeed, road class,
    highway=traffic_signals, highway=speed_camera, barrier=toll_booth).
  * input/custom_alerts/*.csv — the operator's field-surveyed points, merged in
    with spatial de-duplication (<=10 m and heading <=45 deg); the survey wins
    over an OSM point it duplicates.

Node alignment / stitching: segments are built straight from the OSM node graph,
so every shared junction node has ONE canonical coordinate (all ways referencing
that node emit byte-identical start/end E7). That is exactly the normalization
the firmware's RoutePredictor needs — no tile-clipping artifacts (unlike MBTiles).

Outputs (build/speedmap/): tiles.bin, index.bin, metadata.bin, cameras.bin,
signs.bin, names.bin (v2), seg_names.bin (v2), and sounds/ (copied if provided).

Usage:
  python synthesize_and_pack.py [--pbf PATH] [--out DIR] [--sounds-src DIR]
                                [--region VN] [--map-version YYYY.MM.DD.HHMM]
"""
import argparse
import csv
import glob
import math
import os
import shutil
import sys
import time

try:
    import osmium
except ImportError:
    osmium = None

from vhpack import formats as F
from vhpaths import (LOG, DATA_DIR, CUSTOM_ALERTS_DIR, SPEEDMAP_OUT, REGION_CODE)

# ---- OSM tag tables (verbatim from tools/map_builder/build_speedmap.py) ----
DEFAULT_SPEED_BY_HIGHWAY = {
    "motorway": 100, "motorway_link": 60, "trunk": 80, "primary": 80,
    "secondary": 70, "tertiary": 60, "residential": 50, "living_street": 50,
}
ROAD_CLASS_BY_HIGHWAY = {
    "motorway": 1, "motorway_link": 1, "trunk": 1, "trunk_link": 1,
    "primary": 1, "primary_link": 1,
    "secondary": 2, "secondary_link": 2, "tertiary": 2, "tertiary_link": 2,
    "residential": 3, "living_street": 3, "unclassified": 3, "service": 3, "track": 3,
}


def parse_maxspeed_kmh(value):
    if value is None:
        return None
    value = value.strip()
    try:
        return int(round(float(value)))
    except ValueError:
        pass
    if value.lower().endswith("mph"):
        try:
            return int(round(float(value[:-3].strip()) * 1.60934))
        except ValueError:
            return None
    return None


def parse_direction_deg(value):
    if value is None:
        return None
    value = value.strip()
    try:
        return int(round(float(value))) % 360
    except ValueError:
        pass
    compass = {"N": 0, "NNE": 22, "NE": 45, "ENE": 67, "E": 90, "ESE": 112,
               "SE": 135, "SSE": 157, "S": 180, "SSW": 202, "SW": 225, "WSW": 247,
               "W": 270, "WNW": 292, "NW": 315, "NNW": 337}
    return compass.get(value.upper())


def bearing_deg(lat1, lon1, lat2, lon2):
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dlon = math.radians(lon2 - lon1)
    y = math.sin(dlon) * math.cos(phi2)
    x = math.cos(phi1) * math.sin(phi2) - math.sin(phi1) * math.cos(phi2) * math.cos(dlon)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def haversine_m(lat1, lon1, lat2, lon2):
    R = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def angle_diff(a, b):
    if a is None or b is None:
        return 0.0
    d = abs(a - b) % 360.0
    return 360.0 - d if d > 180.0 else d


# ---------------------------------------------------------------------------
if osmium is not None:
    class VietHudOsmHandler(osmium.SimpleHandler):
        def __init__(self):
            super().__init__()
            self.segments = []
            self.seg_name = {}      # segId -> street name (str)
            self.cameras = []
            self.signs = []         # OSM point signs (dicts w/ lat/lon/sign_type/...)
            self.next_seg_id = 1

        def node(self, n):
            if not n.location.valid():
                return
            t = n.tags
            hw = t.get("highway")
            lat, lon = n.location.lat, n.location.lon
            if hw == "speed_camera":
                spd = parse_maxspeed_kmh(t.get("maxspeed"))
                d = parse_direction_deg(t.get("direction"))
                self.cameras.append(dict(id=n.id, lat=lat, lon=lon, speed_limit=spd, direction=d))
                self.signs.append(dict(lat=lat, lon=lon, sign_type=F.SIGN_TYPE_CAMERA,
                                       speed_limit=spd or 0, direction=d, sub_type=0, flags=0))
            elif hw == "traffic_signals":
                d = parse_direction_deg(t.get("traffic_signals:direction") or t.get("direction"))
                self.signs.append(dict(lat=lat, lon=lon, sign_type=F.SIGN_TYPE_TRAFFIC_LIGHT,
                                       speed_limit=0, direction=d, sub_type=0, flags=0))
            elif t.get("barrier") == "toll_booth" or hw == "toll_gantry":
                self.signs.append(dict(lat=lat, lon=lon, sign_type=F.SIGN_TYPE_TOLL_BOOTH,
                                       speed_limit=0, direction=None, sub_type=0, flags=0))

        def way(self, w):
            t = w.tags
            if "highway" not in t:
                return
            coords = []
            for nr in w.nodes:
                loc = nr.location
                coords.append((loc.lat, loc.lon) if loc.valid() else None)
            if len(coords) < 2:
                return
            name = t.get("name:vi") or t.get("name") or t.get("ref")
            hw = t.get("highway")
            road_class = ROAD_CLASS_BY_HIGHWAY.get(hw, 0)
            oneway = t.get("oneway") in ("yes", "true", "1")
            has_cond = "maxspeed:conditional" in t
            fwd, bwd, plain = t.get("maxspeed:forward"), t.get("maxspeed:backward"), t.get("maxspeed")
            base_flags = F.SEGFLAG_HAS_CONDITIONAL if has_cond else 0

            for i in range(len(coords) - 1):
                a, b = coords[i], coords[i + 1]
                if a is None or b is None:
                    continue
                heading = bearing_deg(a[0], a[1], b[0], b[1])

                def make(direction, spd, source):
                    sid = self.next_seg_id
                    self.next_seg_id += 1
                    self.segments.append(dict(
                        id=sid, start=a, end=b, heading=heading, road_class=road_class,
                        direction=direction, speed_limit=spd if spd is not None else -1,
                        source=source, flags=base_flags | (F.SEGFLAG_ONEWAY if oneway else 0),
                        way_id=w.id))
                    if name:
                        self.seg_name[sid] = name

                if fwd is not None or bwd is not None:
                    if fwd is not None:
                        make(F.DIR_FORWARD, parse_maxspeed_kmh(fwd), F.SPEED_SOURCE_OSM_FORWARD)
                    if bwd is not None:
                        make(F.DIR_BACKWARD, parse_maxspeed_kmh(bwd), F.SPEED_SOURCE_OSM_BACKWARD)
                elif plain is not None:
                    make(F.DIR_FORWARD if oneway else F.DIR_BIDIRECTIONAL,
                         parse_maxspeed_kmh(plain), F.SPEED_SOURCE_OSM_MAXSPEED)
                else:
                    dfl = DEFAULT_SPEED_BY_HIGHWAY.get(hw)
                    make(F.DIR_FORWARD if oneway else F.DIR_BIDIRECTIONAL,
                         dfl, F.SPEED_SOURCE_DEFAULT if dfl is not None else F.SPEED_SOURCE_UNKNOWN)


# ---------------------------------------------------------------------------
def load_custom_alerts():
    """Read every input/custom_alerts/*.csv. Columns (header row required):
       lat, lon, sign_type, [speed_limit], [heading|direction], [sub_type]
    sign_type uses the firmware TrafficSignType numbers (1 speed,2 resident,
    3 no-overtake,4 camera,5 toll,6 traffic-light,10 danger). heading is in
    degrees 0..359 (or blank => omnidirectional)."""
    alerts = []
    for path in sorted(glob.glob(str(CUSTOM_ALERTS_DIR / "*.csv"))):
        try:
            with open(path, "r", encoding="utf-8-sig") as f:
                for row in csv.DictReader(f):
                    if not row.get("lat") or not row.get("lon"):
                        continue
                    st = int(float(row.get("sign_type", 0)))
                    hd = row.get("heading") or row.get("direction")
                    alerts.append(dict(
                        lat=float(row["lat"]), lon=float(row["lon"]),
                        sign_type=st,
                        speed_limit=int(float(row["speed_limit"])) if row.get("speed_limit") else 0,
                        direction=parse_direction_deg(hd),
                        sub_type=int(float(row.get("sub_type", 0) or 0)),
                        flags=0, _custom=True))
            LOG.info("custom_alerts: loaded %s", os.path.basename(path))
        except Exception as e:
            LOG.error("custom_alerts: failed to read %s: %s", path, e)
    return alerts


def spatial_dedup(osm_signs, custom_signs, radius_m=10.0, angle_deg=45.0):
    """Merge OSM + custom point signs. When a custom (survey) point duplicates an
    OSM point of the SAME sign_type within radius_m and heading angle_deg, the
    custom one wins and the OSM one is dropped. Returns the merged list."""
    cell = 0.00012  # ~13 m grid
    grid = {}
    for s in osm_signs:
        key = (int(s["lat"] / cell), int(s["lon"] / cell))
        grid.setdefault(key, []).append(s)

    dropped = set()
    for c in custom_signs:
        ck = (int(c["lat"] / cell), int(c["lon"] / cell))
        for dk in ((ck[0] + dx, ck[1] + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
            for s in grid.get(dk, ()):
                if id(s) in dropped or s["sign_type"] != c["sign_type"]:
                    continue
                if haversine_m(c["lat"], c["lon"], s["lat"], s["lon"]) <= radius_m and \
                        angle_diff(c.get("direction"), s.get("direction")) <= angle_deg:
                    dropped.add(id(s))
    merged = [s for s in osm_signs if id(s) not in dropped] + list(custom_signs)
    LOG.info("dedup: %d OSM signs, %d custom, %d OSM dropped as duplicates -> %d total",
             len(osm_signs), len(custom_signs), len(dropped), len(merged))
    return merged


def copy_sounds(sounds_src, out_dir):
    if not sounds_src or not os.path.isdir(sounds_src):
        LOG.warning("sounds source not found (%s) — sounds/ not (re)packed", sounds_src)
        return 0
    dst = os.path.join(out_dir, "sounds")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(sounds_src, dst)
    return sum(len(files) for _, _, files in os.walk(dst))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pbf", default=str(DATA_DIR / "osm" / "vietnam-latest.osm.pbf"))
    ap.add_argument("--out", default=str(SPEEDMAP_OUT))
    ap.add_argument("--sounds-src", default=os.environ.get("VIETHUD_SOUNDS_SRC", ""))
    ap.add_argument("--region", default=REGION_CODE)
    ap.add_argument("--map-version", default=time.strftime("%Y.%m.%d.%H%M"))
    ap.add_argument("--osm-index", default=os.environ.get("VIETHUD_OSM_INDEX", "flex_mem"),
                    help="pyosmium node-location index (flex_mem | sparse_file_array,<path>)")
    args = ap.parse_args()

    if osmium is None:
        LOG.error("pyosmium not installed. pip install osmium  (see requirements.txt)")
        return 1
    if not os.path.exists(args.pbf):
        LOG.error("PBF not found: %s  (run fetch_osm.py first)", args.pbf)
        return 1

    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    LOG.info("parsing OSM %s (index=%s) ...", args.pbf, args.osm_index)
    h = VietHudOsmHandler()
    h.apply_file(args.pbf, locations=True, idx=args.osm_index)
    LOG.info("parsed: %d segments, %d OSM signs, %d cameras in %.0fs",
             len(h.segments), len(h.signs), len(h.cameras), time.time() - t0)

    if not h.segments:
        LOG.error("no road segments parsed — aborting (bad/empty PBF?)")
        return 2

    # Merge operator's surveyed alerts.
    custom = load_custom_alerts()
    custom_cams = [c for c in custom if c["sign_type"] == F.SIGN_TYPE_CAMERA]
    signs = spatial_dedup(h.signs, custom)
    cameras = h.cameras + [dict(id=9_000_000_000 + i, lat=c["lat"], lon=c["lon"],
                                speed_limit=(c["speed_limit"] or None), direction=c.get("direction"))
                           for i, c in enumerate(custom_cams)]

    # Assign final 1-based sign ids and write.
    for i, s in enumerate(signs, start=1):
        s["id"] = i
    max_seg_id = h.next_seg_id - 1

    stats = F.write_network(args.out, h.segments, cameras, args.region, args.map_version)
    F.write_signs(args.out, signs)
    n_names, pool_bytes = F.write_names_v2(args.out, h.seg_name, max_seg_id)
    n_sounds = copy_sounds(args.sounds_src, args.out)

    # counts for the notifier
    n_lights = sum(1 for s in signs if s["sign_type"] == F.SIGN_TYPE_TRAFFIC_LIGHT)
    summary = dict(map_version=args.map_version, region=args.region,
                   tiles=stats["tiles"], segments=stats["segments"],
                   cameras=len(cameras), signs=len(signs), traffic_lights=n_lights,
                   names=n_names, name_pool_bytes=pool_bytes, sounds=n_sounds)
    LOG.info("PACKED %s", summary)

    # Drop a machine-readable summary for github_and_notify.py.
    import json
    (SPEEDMAP_OUT.parent / "last_pack_summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
