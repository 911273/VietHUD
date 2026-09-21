#!/usr/bin/env python3
"""build_speedmap.py — OSM XML -> offline speed-limit database for radar_car.

Runs on a PC, never on the ESP32 (the whole point of this tool — see
src/map/SpeedMapFormat.h's header comment and
C:\\Users\\phamq\\.claude\\plans\\wobbly-swinging-chipmunk.md for why the
firmware must never touch raw OSM data directly). Reads a small OSM XML
(".osm") extract, keeps only the highway/maxspeed-relevant tags, and writes
the binary database src/map/SdCardManager.cpp expects:

    <outdir>/speedmap/metadata.bin
    <outdir>/speedmap/index.bin
    <outdir>/speedmap/tiles.bin (format V2, 2026-09-16 — one packed blob, not
    one file per tile; see SpeedMapFormat.h's TileIndexEntry comment)

Every struct this script packs is documented byte-for-byte in
src/map/SpeedMapFormat.h — the `struct.pack` format strings below MUST stay
in sync with that file's `#pragma pack(push, 1)` structs. There is
deliberately no shared source between the two (Python vs. C++), so a change
to one requires manually re-checking the other; each pack() call below is
commented with the exact SpeedMapFormat.h struct it corresponds to.

V1 scope (see the plan): OSM XML input only (not PBF — see the plan's
"scoping calls" section for why), no maxspeed:conditional evaluation (the
HAS_CONDITIONAL flag is preserved but not acted on), no road classification
beyond the reserved roadClass=0.

Usage:
    python build_speedmap.py sample_region.osm output/ --region VN --map-version 2026.09
"""
import argparse
import csv
import math
import os
from pathlib import Path
import sqlite3
import struct
import sys
import time
import xml.etree.ElementTree as ET
import zlib

MAGIC = b"SLMP"
FORMAT_VERSION = 2  # V2 2026-09-16: packed tiles.bin — see SpeedMapFormat.h's SPEEDMAP_FORMAT_VERSION comment
TILE_SIZE_DEG = 0.01  # must match SpeedMapFormat.h's comment; the firmware reads this from metadata.bin, never hardcodes it

# signs.bin (VNSG) — see SpeedMapFormat.h's TrafficSignHeader/TrafficSignPoint.
SIGN_MAGIC = b"VNSG"
SIGN_FORMAT_VERSION = 1
SIGN_TYPE_CAMERA = 4  # excluded from signs.bin — see write_signs()'s own comment

# SpeedMapFormat.h enums — kept as plain ints here, not a Python enum class,
# so there's no risk of the two languages' enum members drifting out of
# numeric sync without it being obvious (every value below is a literal
# matching the C++ header's own literal ordering).
DIR_BIDIRECTIONAL, DIR_FORWARD, DIR_BACKWARD, DIR_UNKNOWN = 0, 1, 2, 3
SEGFLAG_HAS_CONDITIONAL = 0x01
SEGFLAG_ONEWAY = 0x02
(SPEED_SOURCE_UNKNOWN, SPEED_SOURCE_OSM_MAXSPEED, SPEED_SOURCE_OSM_FORWARD, SPEED_SOURCE_OSM_BACKWARD,
 SPEED_SOURCE_DEFAULT, SPEED_SOURCE_CONDITIONAL) = range(6)

# struct.pack format strings — '<' = little-endian, no alignment padding,
# matching every `#pragma pack(push, 1)` struct in SpeedMapFormat.h exactly.
METADATA_FMT = "<4sH16s16sQIIfiihI64sI"
INDEX_ENTRY_FMT = "<IiiiiII"
SEGMENT_FMT = "<IiiiiHBBhBB"
CAMERA_FMT = "<QiihH"  # Q (uint64), not I (uint32) — real OSM node ids already exceed 2^32, confirmed against real Vietnam data (see CameraPoint's own SpeedMapFormat.h comment)
SIGN_HEADER_FMT = "<4sHIHII"  # TrafficSignHeader: magic,version,signCount,reserved,crc32,timestamp
SIGN_POINT_FMT = "<IiiHBBBBH"  # TrafficSignPoint: id,latE7,lonE7,directionDeg,signType,speedLimitKmh,subType,flags,reserved

assert struct.calcsize(METADATA_FMT) == 140, "METADATA_FMT drifted from SpeedMapFormat.h's SpeedMapMetadata"
assert struct.calcsize(INDEX_ENTRY_FMT) == 28, "INDEX_ENTRY_FMT drifted from SpeedMapFormat.h's TileIndexEntry"
assert struct.calcsize(SEGMENT_FMT) == 28, "SEGMENT_FMT drifted from SpeedMapFormat.h's RoadSegment"
assert struct.calcsize(CAMERA_FMT) == 20, "CAMERA_FMT drifted from SpeedMapFormat.h's CameraPoint"
assert struct.calcsize(SIGN_HEADER_FMT) == 20, "SIGN_HEADER_FMT drifted from SpeedMapFormat.h's TrafficSignHeader"
assert struct.calcsize(SIGN_POINT_FMT) == 20, "SIGN_POINT_FMT drifted from SpeedMapFormat.h's TrafficSignPoint"


def to_e7(deg):
    return int(round(deg * 1e7))


def tile_id(lat_deg, lon_deg):
    """Must compute the IDENTICAL id speedmapTileId() does in
    SpeedMapFormat.h — re-implemented here rather than shared source since
    one side is C++ compiled into firmware and the other is a throwaway PC
    script; keeping the formula textually side-by-side in both files (each
    referencing the other by name in a comment) is deliberately preferred
    over a code-generation step for something this small and this rarely
    changed."""
    lat_cell = int((lat_deg + 90.0) / TILE_SIZE_DEG)
    lon_cell = int((lon_deg + 180.0) / TILE_SIZE_DEG)
    return ((lat_cell & 0xFFFF) << 16) | (lon_cell & 0xFFFF)


def bearing_deg(lat1, lon1, lat2, lon2):
    """Initial great-circle bearing from (lat1,lon1) to (lat2,lon2), 0-359,
    0=North/clockwise — matches RoadSegment::headingDeg's documented meaning
    in SpeedMapFormat.h. Fine-grained geodesic precision doesn't matter at
    the segment lengths this format targets (tens to low hundreds of
    meters); the simple spherical formula is plenty."""
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dlon = math.radians(lon2 - lon1)
    y = math.sin(dlon) * math.cos(phi2)
    x = math.cos(phi1) * math.sin(phi2) - math.sin(phi1) * math.cos(phi2) * math.cos(dlon)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def parse_maxspeed_kmh(value):
    """Only plain numeric km/h and a "<number> mph" suffix are handled in
    V1 — anything else (a country-code implicit limit like "RU:urban", a
    zone reference, garbage) returns None, which becomes speed_limit=-1
    (UNKNOWN) rather than a guessed number. Spec section 4: never invent a
    value the source data doesn't actually support."""
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



# Vietnam default speed limits by OSM highway=* class (user-requested
# 2026-09-21, "nang cap build_speedmap.py voi Luat toc do Viet Nam") —
# Thong tu 31/2019/TT-BGTVT's own general road-class speed-limit table
# (a DIFFERENT table from the same circular's vehicle-following-distance
# one that src/core/SafeDistanceRules.h already implements on the firmware
# side). Applied ONLY when a way has no maxspeed/maxspeed:forward/
# maxspeed:backward tag at all (see build_segments()'s else branch below) —
# real signposted maxspeed always wins over this. Real-world OSM tagging
# for Vietnam leaves the large majority of ways untagged, which previously
# meant SPEED_SOURCE_UNKNOWN (blank speed-limit sign) for almost the whole
# map; this is what src/map/SpeedMapFormat.h's SPEED_SOURCE_DEFAULT enum
# value and RoadSegment.speedLimitKmh's own "-1 = ... no applicable
# default" comment were already anticipating; the firmware side
# (SpeedLimitManager.cpp's speedSourceStr()) already handles it, only this
# script was never updated to actually produce it.
#
# Where the circular gives a RANGE (motorway 100-120, trunk 80-90), the
# LOWER bound is used — a safety-conservative choice for an assumed
# default: an assumed limit that's too LOW only makes the speeding/
# following-distance warnings fire a bit early, never late, if the real
# posted sign turns out higher. Same "err conservative on an unproven
# default" reasoning this project already applies elsewhere
# (SafeDistanceRules.h's singleton-row handling, LD2451.cpp's un-tuned
# harsh-brake threshold).
#
# Only the highway=* classes the circular's table actually names are
# covered here — anything else (service, track, pedestrian, unclassified,
# steps, ...) stays a genuine UNKNOWN rather than guessing at a class the
# source regulation doesn't cover.
DEFAULT_SPEED_BY_HIGHWAY = {
    "motorway": 100,
    "motorway_link": 60,
    "trunk": 80,
    "primary": 80,
    "secondary": 70,
    "tertiary": 60,
    "residential": 50,
    "living_street": 50,
}


class Way:
    def __init__(self, way_id, node_ids, tags):
        self.id = way_id
        self.node_ids = node_ids
        self.tags = tags


def parse_direction_deg(value):
    """OSM direction=* is usually plain degrees, occasionally a cardinal
    compass point (N/NE/E/...) — spec section for CameraPoint.directionDeg
    only needs coarse degrees, so a small compass table covers the
    non-numeric case without pulling in a full parsing library for it.
    Returns None (-> 0xFFFF, UNKNOWN/omnidirectional) for anything else."""
    if value is None:
        return None
    value = value.strip()
    try:
        return int(round(float(value))) % 360
    except ValueError:
        pass
    compass = {"N": 0, "NNE": 22, "NE": 45, "ENE": 67, "E": 90, "ESE": 112, "SE": 135, "SSE": 157,
               "S": 180, "SSW": 202, "SW": 225, "WSW": 247, "W": 270, "WNW": 292, "NW": 315, "NNW": 337}
    return compass.get(value.upper())


def parse_osm(path):
    tree = ET.parse(path)
    root = tree.getroot()
    nodes = {}
    cameras = []
    for n in root.findall("node"):
        lat, lon = float(n.get("lat")), float(n.get("lon"))
        nodes[n.get("id")] = (lat, lon)
        tags = {t.get("k"): t.get("v") for t in n.findall("tag")}
        # highway=speed_camera (user-requested 2026-09-21, "tai du lieu ve
        # canh bao giao thong, gom camera") — a plain node tag, not a way,
        # so this lives in the node loop rather than the way loop below.
        # See CameraPoint's own comment in SpeedMapFormat.h for why OSM
        # (not Waze) is the source.
        if tags.get("highway") == "speed_camera":
            cameras.append(dict(
                id=int(n.get("id")),
                lat=lat, lon=lon,
                speed_limit=parse_maxspeed_kmh(tags.get("maxspeed")),
                direction=parse_direction_deg(tags.get("direction")),
            ))

    ways = []
    for w in root.findall("way"):
        tags = {t.get("k"): t.get("v") for t in w.findall("tag")}
        if "highway" not in tags:
            continue  # not a road — irrelevant to a speed-limit map (spec section 3)
        node_ids = [nd.get("ref") for nd in w.findall("nd")]
        ways.append(Way(int(w.get("id")), node_ids, tags))  # int, not str — build_database() compares this against --test-way-id
    return nodes, ways, cameras


def build_segments(nodes, ways):
    """Splits each way into one RoadSegment per consecutive node pair (not
    one per whole way) so heading stays accurate along a curved road, and
    emits 1 or 2 RoadSegments per pair depending on whether the way has a
    single maxspeed or a forward/backward split (spec section 13)."""
    segments = []
    next_id = 1
    for way in ways:
        tags = way.tags
        oneway = tags.get("oneway") in ("yes", "true", "1")
        has_conditional = "maxspeed:conditional" in tags
        fwd_tag, bwd_tag, plain_tag = tags.get("maxspeed:forward"), tags.get("maxspeed:backward"), tags.get(
            "maxspeed")

        for i in range(len(way.node_ids) - 1):
            a, b = way.node_ids[i], way.node_ids[i + 1]
            if a not in nodes or b not in nodes:
                print(f"  WARN way {way.id}: node {a if a not in nodes else b} missing, skipping this sub-segment",
                      file=sys.stderr)
                continue
            lat1, lon1 = nodes[a]
            lat2, lon2 = nodes[b]
            heading = bearing_deg(lat1, lon1, lat2, lon2)
            flags = SEGFLAG_HAS_CONDITIONAL if has_conditional else 0

            def make(direction, speed_kmh, source):
                nonlocal next_id
                seg = dict(id=next_id, start=(lat1, lon1), end=(lat2, lon2), heading=heading, road_class=0,
                           direction=direction, speed_limit=speed_kmh if speed_kmh is not None else -1,
                           source=source, flags=flags | (SEGFLAG_ONEWAY if oneway else 0), way_id=way.id)
                next_id += 1
                segments.append(seg)
                return seg

            if fwd_tag is not None or bwd_tag is not None:
                if fwd_tag is not None:
                    make(DIR_FORWARD, parse_maxspeed_kmh(fwd_tag), SPEED_SOURCE_OSM_FORWARD)
                if bwd_tag is not None:
                    make(DIR_BACKWARD, parse_maxspeed_kmh(bwd_tag), SPEED_SOURCE_OSM_BACKWARD)
            elif plain_tag is not None:
                direction = DIR_FORWARD if oneway else DIR_BIDIRECTIONAL
                make(direction, parse_maxspeed_kmh(plain_tag), SPEED_SOURCE_OSM_MAXSPEED)
            else:
                direction = DIR_FORWARD if oneway else DIR_BIDIRECTIONAL
                default_kmh = DEFAULT_SPEED_BY_HIGHWAY.get(tags.get("highway"))
                if default_kmh is not None:
                    make(direction, default_kmh, SPEED_SOURCE_DEFAULT)
                else:
                    make(direction, None, SPEED_SOURCE_UNKNOWN)

    return segments


def pack_segment(seg):
    return struct.pack(
        SEGMENT_FMT,
        seg["id"],
        to_e7(seg["start"][0]), to_e7(seg["start"][1]),
        to_e7(seg["end"][0]), to_e7(seg["end"][1]),
        int(round(seg["heading"])) % 360,
        seg["road_class"],
        seg["direction"],
        seg["speed_limit"],
        seg["source"],
        seg["flags"],
    )


def pack_camera(cam):
    return struct.pack(
        CAMERA_FMT,
        cam["id"],
        to_e7(cam["lat"]), to_e7(cam["lon"]),
        cam["speed_limit"] if cam["speed_limit"] is not None else -1,
        cam["direction"] if cam["direction"] is not None else 0xFFFF,
    )



def haversine_m(lat1, lon1, lat2, lon2):
    """Distance in meters between two lat/lon points."""
    R = 6371000.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2.0) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2.0) ** 2
    return 2.0 * R * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))


def angle_diff(a, b):
    """Smallest absolute difference between two angles in degrees [0, 180]."""
    d = abs(a - b) % 360.0
    return 360.0 - d if d > 180.0 else d


def merge_wyn_data(segments, cameras, wyn_network_path=None, wyn_signs_path=None,
                   match_radius_m=25.0, match_angle_deg=35.0):
    """Enriches OSM road segments that lack explicit OSM maxspeed tags by matching them
    against the WYN road network and WYN traffic signs. Also injects traffic enforcement
    cameras from WYN into the cameras list for cameras.bin.
    """
    if not segments or (not wyn_network_path and not wyn_signs_path):
        return 0, 0

    # Calculate bounding box of the OSM segments
    lats = [s["start"][0] for s in segments] + [s["end"][0] for s in segments]
    lons = [s["start"][1] for s in segments] + [s["end"][1] for s in segments]
    min_lat, max_lat = min(lats) - 0.01, max(lats) + 0.01
    min_lon, max_lon = min(lons) - 0.01, max(lons) + 0.01

    GRID_SIZE = 0.002  # ~200m spatial hash cell
    grid = {}  # (gx, gy) -> list of (lat, lon, heading, speed)

    added_cameras = 0
    # 1. Load WYN Signs if provided
    if wyn_signs_path:
        signs_file = Path(wyn_signs_path)
        if not signs_file.exists():
            print(f"  WARN: WYN signs file not found: {wyn_signs_path}", file=sys.stderr)
        else:
            print(f"Loading WYN traffic signs from {signs_file} ...")
            cam_synthetic_id = 9000000000
            with open(signs_file, "r", encoding="utf-8-sig") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    lat = float(row["lat"])
                    lon = float(row["lon"])
                    if not (min_lat <= lat <= max_lat and min_lon <= lon <= max_lon):
                        continue

                    sign_type = row.get("sign_type", "")
                    speed = int(row.get("speed_limit", 0))
                    heading = float(row.get("heading", 0))

                    # Type 4 = Speed camera / enforcement camera
                    if sign_type == "4":
                        cameras.append(dict(
                            id=cam_synthetic_id,
                            lat=lat,
                            lon=lon,
                            speed_limit=speed if speed > 0 else None,
                            direction=int(round(heading * 2.0)) % 360 if heading > 0 else None,
                        ))
                        cam_synthetic_id += 1
                        added_cameras += 1

                    # Index speed signs
                    if speed > 0:
                        gx = int(lat / GRID_SIZE)
                        gy = int(lon / GRID_SIZE)
                        grid.setdefault((gx, gy), []).append((lat, lon, heading * 2.0, speed))

    # 2. Load WYN Road Network if provided
    if wyn_network_path:
        net_path = Path(wyn_network_path)
        db_cand = net_path.with_name("wmap_network.db") if net_path.suffix == ".wmap" else net_path
        if not db_cand.exists() and net_path.exists() and net_path.suffix == ".db":
            db_cand = net_path

        if db_cand.exists() and db_cand.suffix == ".db":
            print(f"Querying WYN road network from SQLite: {db_cand} ...")
            conn = sqlite3.connect(str(db_cand))
            cur = conn.cursor()
            cur.execute("""
                SELECT n1.lat, n1.lon, n2.lat, n2.lon, e.speed_limit, e.heading
                FROM edges e
                JOIN nodes n1 ON e.u = n1.id
                JOIN nodes n2 ON e.v = n2.id
                WHERE n1.lat BETWEEN ? AND ? AND n1.lon BETWEEN ? AND ?
                  AND e.speed_limit > 0
            """, (min_lat, max_lat, min_lon, max_lon))
            edges = cur.fetchall()
            conn.close()
            print(f"  Loaded {len(edges):,} relevant WYN road edges in BBox")

            for lat1, lon1, lat2, lon2, speed, heading in edges:
                mid_lat = (lat1 + lat2) / 2.0
                mid_lon = (lon1 + lon2) / 2.0
                gx = int(mid_lat / GRID_SIZE)
                gy = int(mid_lon / GRID_SIZE)
                grid.setdefault((gx, gy), []).append((mid_lat, mid_lon, heading, speed))
        elif net_path.exists() and net_path.suffix == ".wmap":
            print(f"  NOTE: To query {net_path.name} faster, ensure wmap_network.db is generated.")
        else:
            print(f"  WARN: WYN network file not found: {wyn_network_path}", file=sys.stderr)

    if not grid:
        return 0, added_cameras

    # 3. Match against OSM segments lacking real maxspeed tags (source in DEFAULT, UNKNOWN)
    updated_segments = 0
    for s in segments:
        if s["source"] in (SPEED_SOURCE_DEFAULT, SPEED_SOURCE_UNKNOWN):
            mid_lat = (s["start"][0] + s["end"][0]) / 2.0
            mid_lon = (s["start"][1] + s["end"][1]) / 2.0
            s_heading = s["heading"]

            gx = int(mid_lat / GRID_SIZE)
            gy = int(mid_lon / GRID_SIZE)

            best_speed = None
            best_dist = match_radius_m

            for dgx in (-1, 0, 1):
                for dgy in (-1, 0, 1):
                    cell = grid.get((gx + dgx, gy + dgy))
                    if not cell:
                        continue
                    for c_lat, c_lon, c_heading, c_speed in cell:
                        if angle_diff(s_heading, c_heading) <= match_angle_deg:
                            dist = haversine_m(mid_lat, mid_lon, c_lat, c_lon)
                            if dist < best_dist:
                                best_dist = dist
                                best_speed = c_speed

            if best_speed is not None:
                if s["speed_limit"] != best_speed:
                    s["speed_limit"] = best_speed
                    s["source"] = SPEED_SOURCE_DEFAULT  # verified speed limit
                    updated_segments += 1

    print(f"WYN Merge completed:")
    if added_cameras:
        print(f"  + Added {added_cameras:,} speed camera(s) from WYN into cameras.bin")
    print(f"  + Refined {updated_segments:,} segment(s) with verified WYN speed limits")
    return updated_segments, added_cameras


def write_signs(csv_path, out_dir):
    """Reads the WYN traffic-signs CSV (lat,lon,speed_limit,heading,sign_type)
    and writes <out_dir>/speedmap/signs.bin (VNSG format — see
    SpeedMapFormat.h's TrafficSignHeader/TrafficSignPoint). Called from
    main() when --wyn-signs is given — same CSV merge_wyn_data() already
    reads for camera injection / OSM-segment speed enrichment, read a second
    time here since this pass writes a DIFFERENT output file with different
    filtering (every non-camera sign type, not just speed-bearing ones).

    Sign type 4 (camera) rows are deliberately EXCLUDED here — merge_wyn_data()
    already routes them into cameras.bin (richer CameraPoint fields), and
    writing them into signs.bin too would duplicate every camera in both
    files. Verified against the real CSV: sign_type distribution is
    {1:15494, 2:5183, 3:4333, 4:8075, 5:3050, 6:1433, 10:772} — signs.bin
    ends up with the ~30k non-camera rows, cameras.bin (via merge_wyn_data)
    gets the ~8k camera rows.
    """
    signs_file = Path(csv_path)
    if not signs_file.exists():
        print(f"  WARN: WYN signs file not found for signs.bin: {csv_path}", file=sys.stderr)
        return 0

    points = b""
    next_id = 1
    skipped_camera = 0
    with open(signs_file, "r", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            sign_type = int(row.get("sign_type", 0))
            if sign_type == SIGN_TYPE_CAMERA:
                skipped_camera += 1
                continue  # already handled by merge_wyn_data() -> cameras.bin

            lat = float(row["lat"])
            lon = float(row["lon"])
            heading = float(row.get("heading", 0) or 0)
            # Same raw/half-bearing encoding merge_wyn_data() already applies
            # to type-4 (camera) rows from this same CSV — verified the
            # heading column is 0-180 for every sign type, not just cameras.
            direction_deg = int(round(heading * 2.0)) % 360
            speed_limit = int(row.get("speed_limit", 0) or 0)
            speed_limit = max(0, min(255, speed_limit))  # clamp to uint8 — observed values are already small (0, 40, 50, 60, ...)

            # subType: verified the CSV has NO reliable start/end signal —
            # speed_limit==0 rows exist across every sign type at rates
            # (33-76%) far too high to represent "end of zone" markers; it
            # just means no numeric speed value applies to that row (true
            # inherently for toll/light/danger, apparently untagged for many
            # no-overtaking/resident rows too). Rather than fabricate a
            # start/end distinction the source data doesn't support (see
            # SharedState.h's RoadInfoSnapshot "HONESTY NOTE"), every row is
            # written as subType=0 (start/active) uniformly.
            points += struct.pack(
                SIGN_POINT_FMT,
                next_id,
                to_e7(lat), to_e7(lon),
                direction_deg,
                sign_type,
                speed_limit,
                0,  # subType — see comment above
                0,  # flags
                0,  # reserved
            )
            next_id += 1

    sign_count = next_id - 1
    speedmap_dir = os.path.join(out_dir, "speedmap")
    os.makedirs(speedmap_dir, exist_ok=True)
    # CRC32 over the packed TrafficSignPoint[] bytes only (not the header
    # itself) — same convention metadata.bin's own crc32 field uses for
    # index.bin (see SpeedMapFormat.h's SpeedMapMetadata::crc32 comment).
    points_crc = zlib.crc32(points) & 0xFFFFFFFF
    header = struct.pack(SIGN_HEADER_FMT, SIGN_MAGIC, SIGN_FORMAT_VERSION, sign_count, 0, points_crc, int(time.time()))
    with open(os.path.join(speedmap_dir, "signs.bin"), "wb") as f:
        f.write(header)
        f.write(points)

    print(f"Wrote {sign_count} traffic sign(s) to {out_dir}/speedmap/signs.bin "
          f"(skipped {skipped_camera} camera row(s), already in cameras.bin) crc32=0x{points_crc:08X}")
    return sign_count


def build_database(segments, out_dir, region, map_version, test_way_id, cameras=()):
    tiles = {}
    for seg in segments:
        tid = tile_id(*seg["start"])
        tiles.setdefault(tid, []).append(seg)

    speedmap_dir = os.path.join(out_dir, "speedmap")
    os.makedirs(speedmap_dir, exist_ok=True)

    # cameras.bin — flat, unindexed (see its own SpeedMapFormat.h comment
    # for why). Always written, even when empty (an empty file, not a
    # missing one) — SdCardManager.cpp's open() treating "missing" and
    # "empty" identically (both -> zero cameras) means this doesn't need
    # its own special case either way, but writing it consistently avoids
    # a stale cameras.bin from an EARLIER build (with cameras this one no
    # longer has, e.g. after filtering source data down) lingering in an
    # out_dir that gets reused across runs.
    with open(os.path.join(speedmap_dir, "cameras.bin"), "wb") as f:
        for cam in cameras:
            f.write(pack_camera(cam))

    # Format V2 (2026-09-16): one packed tiles.bin, not one file per tile —
    # see SpeedMapFormat.h's TileIndexEntry comment for the real SD-card
    # cluster-size wall that forced this (a 62081-tile Northern-Vietnam
    # extract needed ~970MB of slack space alone on a 16KB-cluster card,
    # for only ~74MB of actual tile data). Each tile's bytes are appended
    # back to back; fileOffset now actually means something.
    index_entries = b""
    offset = 0
    tiles_bin_path = os.path.join(speedmap_dir, "tiles.bin")
    with open(tiles_bin_path, "wb") as tiles_f:
        for tid, segs in sorted(tiles.items()):
            lats = [s["start"][0] for s in segs] + [s["end"][0] for s in segs]
            lons = [s["start"][1] for s in segs] + [s["end"][1] for s in segs]
            tile_bytes = b"".join(pack_segment(s) for s in segs)
            tiles_f.write(tile_bytes)
            index_entries += struct.pack(
                INDEX_ENTRY_FMT,
                tid,
                to_e7(min(lats)), to_e7(max(lats)),
                to_e7(min(lons)), to_e7(max(lons)),
                offset,
                len(tile_bytes),
            )
            offset += len(tile_bytes)

    with open(os.path.join(out_dir, "speedmap", "index.bin"), "wb") as f:
        f.write(index_entries)
    index_crc = zlib.crc32(index_entries) & 0xFFFFFFFF

    test_lat_e7, test_lon_e7, test_heading, test_road_id = 0, 0, 0, 0
    if test_way_id is not None:
        candidates = [s for s in segments if s["way_id"] == test_way_id]
        if candidates:
            s = candidates[0]
            mid_lat = (s["start"][0] + s["end"][0]) / 2.0
            mid_lon = (s["start"][1] + s["end"][1]) / 2.0
            test_lat_e7, test_lon_e7 = to_e7(mid_lat), to_e7(mid_lon)
            test_heading = int(round(s["heading"])) % 360
            test_road_id = s["id"]
        else:
            print(f"  WARN --test-way-id {test_way_id} matched no segment — self-test point left empty",
                  file=sys.stderr)

    attribution = b"(c) OpenStreetMap contributors, ODbL - openstreetmap.org/copyright"
    metadata = struct.pack(
        METADATA_FMT,
        MAGIC,
        FORMAT_VERSION,
        region.encode("ascii")[:16],
        map_version.encode("ascii")[:16],
        int(time.time()),
        len(tiles),
        len(segments),
        TILE_SIZE_DEG,
        test_lat_e7, test_lon_e7, test_heading, test_road_id,
        attribution[:64],
        index_crc,
    )
    with open(os.path.join(out_dir, "speedmap", "metadata.bin"), "wb") as f:
        f.write(metadata)

    print(f"Wrote {len(tiles)} tile(s), {len(segments)} segment(s), {len(cameras)} speed camera(s) to "
          f"{out_dir}/speedmap/")
    print(f"  region={region} map_version={map_version} index_crc32=0x{index_crc:08X}")
    if test_road_id:
        print(f"  self-test point: way {test_way_id} -> road_id={test_road_id} "
              f"lat={test_lat_e7/1e7:.7f} lon={test_lon_e7/1e7:.7f} heading={test_heading}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("osm_file", help="Input OSM XML (.osm) file")
    ap.add_argument("out_dir", help="Output directory — contents get written to <out_dir>/speedmap/")
    ap.add_argument("--region", default="XX", help="Free-text region code, max 16 ASCII chars (default: XX)")
    ap.add_argument("--map-version", default=time.strftime("%Y.%m"), help="Free-text map version, max 16 ASCII chars")
    ap.add_argument("--test-way-id", default=None,
                     help="OSM way id to embed as the self-test point (metadata.bin's testLat/testLon/testRoadId) "
                          "- SpeedLimitManager.cpp matches against this at boot to verify the whole pipeline "
                          "without needing a live GNSS fix. Omit to skip embedding a self-test point.")
    ap.add_argument("--wyn-network", default=None,
                    help="Path to WYN road network SQLite DB (wmap_network.db) or data.wmap to enrich missing speeds")
    ap.add_argument("--wyn-signs", default=None,
                    help="Path to WYN traffic signs CSV (traffic_signs.csv) to enrich missing speeds and cameras")
    ap.add_argument("--match-radius-m", type=float, default=25.0,
                    help="Maximum distance in meters to match an OSM segment to a WYN road/sign (default: 25.0)")
    ap.add_argument("--match-angle-deg", type=float, default=35.0,
                    help="Maximum heading difference in degrees to match an OSM segment (default: 35.0)")
    args = ap.parse_args()

    print(f"Parsing {args.osm_file} ...")
    nodes, ways, cameras = parse_osm(args.osm_file)
    print(f"  {len(nodes)} node(s), {len(ways)} highway way(s), {len(cameras)} speed_camera node(s)")

    segments = build_segments(nodes, ways)

    if args.wyn_network or args.wyn_signs:
        merge_wyn_data(segments, cameras, args.wyn_network, args.wyn_signs,
                       args.match_radius_m, args.match_angle_deg)

    unknown = sum(1 for s in segments if s["speed_limit"] < 0)
    defaulted = sum(1 for s in segments if s["source"] == SPEED_SOURCE_DEFAULT)
    tagged = len(segments) - unknown - defaulted
    coverage = 100.0 * (len(segments) - unknown) / len(segments) if segments else 0.0
    print(f"  {len(segments)} road segment(s) built: {tagged} from real OSM maxspeed tags, "
          f"{defaulted} with default/WYN speed limits, {unknown} still UNKNOWN "
          f"({coverage:.1f}% speed-limit coverage)")

    build_database(segments, args.out_dir, args.region, args.map_version,
                    int(args.test_way_id) if args.test_way_id is not None else None, cameras)

    if args.wyn_signs:
        write_signs(args.wyn_signs, args.out_dir)


if __name__ == "__main__":
    main()
