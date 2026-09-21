#!/usr/bin/env python3
"""test_matcher.py — PC-side dry run of the map-matching algorithm
implemented in src/map/SpeedLimitManager.cpp, covering several of spec
section 43's required test scenarios without needing real ESP32 hardware or
a live GNSS fix (this project has not seen one in any session yet).

IMPORTANT: this is a Python PORT of the C++ matching logic (evaluateSegment,
runMatch, the tile-id packing), not a shared implementation — every function
below names the C++ function it mirrors. If SpeedLimitManager.cpp's
algorithm or constants change, this file needs the same change made by hand
to stay meaningful; a passing run here is evidence the ALGORITHM's logic is
sound, not proof the C++ implementation matches it byte-for-byte (build,
flash, and read map/SpeedLimitManager.cpp's own [map] self-test log line
for that).

Usage (after build_speedmap.py has produced a database):
    python build_speedmap.py sample_region.osm output --test-way-id 100
    python test_matcher.py output/speedmap
"""
import math
import os
import struct
import sys

TILE_SIZE_DEG = 0.01
MAX_MATCH_DISTANCE_M = 30.0
MIN_CONFIDENCE = 0.5
SWITCH_MARGIN = 0.15
CONTINUITY_BONUS = 0.1

SEGFLAG_ONEWAY = 0x02
DIR_FORWARD, DIR_BACKWARD = 1, 2

METADATA_FMT = "<4sH16s16sQIIfiihI64sI"
INDEX_ENTRY_FMT = "<IiiiiII"
SEGMENT_FMT = "<IiiiiHBBhBB"


def load_database(speedmap_dir):
    with open(os.path.join(speedmap_dir, "metadata.bin"), "rb") as f:
        meta = f.read()
    (magic, fmt_ver, region, map_ver, ts, tile_count, seg_count, tile_size, test_lat_e7, test_lon_e7, test_heading,
     test_road_id, attribution, crc) = struct.unpack(METADATA_FMT, meta)
    assert magic == b"SLMP", "not a speedmap database"
    # Mirrors SdCardManager.cpp's own mount() check: a formatVersion other
    # than SPEEDMAP_FORMAT_VERSION (2) means this dataset predates the
    # single-tiles.bin-blob layout below and the firmware itself would
    # refuse to load it (MAP FORMAT ERROR) rather than silently mis-read it.
    assert fmt_ver == 2, f"unsupported formatVersion {fmt_ver} (this script only reads V2 — packed tiles.bin)"

    with open(os.path.join(speedmap_dir, "index.bin"), "rb") as f:
        idx_bytes = f.read()
    entries = [struct.unpack(INDEX_ENTRY_FMT, idx_bytes[i:i + 28]) for i in range(0, len(idx_bytes), 28)]

    # V2 (2026-09-16): every tile's RoadSegment[] lives back-to-back in one
    # /speedmap/tiles.bin blob, sliced by each TileIndexEntry's fileOffset/
    # fileSize (see SpeedMapFormat.h's own header comment for why the old
    # one-file-per-tile layout — tiles/tile_NNNNNN.bin — was retired: it hit
    # a hard SD-card cluster-rounding wall on a real 62081-tile extract).
    # Read once, not per-tile: a real Northern-Vietnam export is ~74MB, so
    # this stays one sequential read instead of 62081 tiny file opens.
    with open(os.path.join(speedmap_dir, "tiles.bin"), "rb") as f:
        tiles_blob = f.read()

    segments_by_tile = {}
    for (tid, minlat, maxlat, minlon, maxlon, off, size) in entries:
        data = tiles_blob[off:off + size]
        segs = []
        for i in range(0, len(data), 28):
            (sid, slat, slon, elat, elon, heading, road_class, direction, speed, source, flags) = \
                struct.unpack(SEGMENT_FMT, data[i:i + 28])
            segs.append(dict(id=sid, start=(slat / 1e7, slon / 1e7), end=(elat / 1e7, elon / 1e7), heading=heading,
                              direction=direction, speed_limit=speed, source=source, flags=flags))
        segments_by_tile[tid] = segs

    return dict(tile_size=tile_size, test_point=(test_lat_e7 / 1e7, test_lon_e7 / 1e7, test_heading, test_road_id),
                segments_by_tile=segments_by_tile)


def to_local_m(lat, lon, origin_lat, origin_lon):
    y = (lat - origin_lat) * 110540.0
    x = (lon - origin_lon) * 111320.0 * math.cos(math.radians(origin_lat))
    return x, y


def point_segment_distance_m(fix_lat, fix_lon, start, end):
    """Mirrors SpeedLimitManager.cpp's pointSegmentDistanceM()."""
    px, py = to_local_m(fix_lat, fix_lon, *start)
    ex, ey = to_local_m(end[0], end[1], *start)
    seg_len_sq = ex * ex + ey * ey
    t = (px * ex + py * ey) / seg_len_sq if seg_len_sq > 1e-6 else 0.0
    t = max(0.0, min(1.0, t))
    dx, dy = px - ex * t, py - ey * t
    return math.hypot(dx, dy)


def angular_diff_deg(a, b):
    d = abs(a - b) % 360.0
    return 360.0 - d if d > 180.0 else d


def pack_tile(lat_cell, lon_cell):
    return ((lat_cell & 0xFFFF) << 16) | (lon_cell & 0xFFFF)


def evaluate_segment(seg, fix_lat, fix_lon, heading_valid, heading_deg):
    """Mirrors SpeedLimitManager.cpp's evaluateSegment() — same rules, same
    order (distance gate -> heading error -> oneway disqualify -> confidence
    formula), see that function's comments for the reasoning."""
    dist = point_segment_distance_m(fix_lat, fix_lon, seg["start"], seg["end"])
    if dist > MAX_MATCH_DISTANCE_M:
        return None

    fwd, bwd = seg["heading"], (seg["heading"] + 180) % 360
    heading_err = 0.0
    if heading_valid:
        if seg["direction"] == DIR_FORWARD:
            heading_err = angular_diff_deg(heading_deg, fwd)
        elif seg["direction"] == DIR_BACKWARD:
            heading_err = angular_diff_deg(heading_deg, bwd)
        else:
            heading_err = min(angular_diff_deg(heading_deg, fwd), angular_diff_deg(heading_deg, bwd))

    if (seg["flags"] & SEGFLAG_ONEWAY) and heading_valid and heading_err > 90.0:
        return None

    if heading_valid:
        confidence = 1.0 - dist / MAX_MATCH_DISTANCE_M - heading_err / 90.0
    else:
        confidence = min(1.0 - dist / MAX_MATCH_DISTANCE_M, 0.6)
    confidence = max(0.0, min(1.0, confidence))

    return dict(road_id=seg["id"], distance=dist, confidence=confidence, speed_limit=seg["speed_limit"],
                source=seg["source"])


class Matcher:
    """Mirrors the continuity/hysteresis state SpeedLimitManager.cpp keeps
    across task-loop ticks (currentRoadId, lastPublished) — one instance per
    simulated vehicle across a sequence of match() calls."""

    def __init__(self, db):
        self.db = db
        self.current_road_id = 0
        self.last_valid = None

    def match(self, lat, lon, heading_valid, heading_deg):
        tsize = self.db["tile_size"]
        lat_cell = int((lat + 90.0) / tsize)
        lon_cell = int((lon + 180.0) / tsize)
        tile_ids = [pack_tile(lat_cell, lon_cell), pack_tile(lat_cell + 1, lon_cell),
                    pack_tile(lat_cell - 1, lon_cell), pack_tile(lat_cell, lon_cell + 1),
                    pack_tile(lat_cell, lon_cell - 1)]

        best = None
        best_for_current = None
        for tid in tile_ids:
            for seg in self.db["segments_by_tile"].get(tid, []):
                cand = evaluate_segment(seg, lat, lon, heading_valid, heading_deg)
                if cand is None:
                    continue
                if cand["road_id"] == self.current_road_id:
                    best_for_current = cand
                if best is None or cand["confidence"] > best["confidence"]:
                    best = cand

        chosen = None
        if best is not None:
            if (best_for_current is not None and
                    (best_for_current["confidence"] + CONTINUITY_BONUS) >= (best["confidence"] - SWITCH_MARGIN)):
                chosen = best_for_current
            else:
                chosen = best

        if chosen is None or chosen["confidence"] < MIN_CONFIDENCE:
            if self.last_valid is not None:
                return dict(self.last_valid, held=True)
            return dict(valid=False, road_id=0, speed_limit=-1, source=None, confidence=0.0, held=False)

        self.current_road_id = chosen["road_id"]
        result = dict(valid=chosen["speed_limit"] >= 0, road_id=chosen["road_id"],
                      speed_limit=chosen["speed_limit"] if chosen["speed_limit"] >= 0 else -1,
                      source=chosen["source"], confidence=chosen["confidence"], held=False)
        self.last_valid = result
        return result


def check(label, cond):
    print(f"[{'PASS' if cond else 'FAIL'}] {label}")
    return cond


def main():
    speedmap_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join("output", "speedmap")
    db = load_database(speedmap_dir)
    all_ok = True

    # Spec test 1: the embedded self-test point must match its own road.
    test_lat, test_lon, test_heading, test_road_id = db["test_point"]
    if test_road_id:
        r = Matcher(db).match(test_lat, test_lon, True, test_heading)
        all_ok &= check(f"Test 1 - self-test point matches road {test_road_id} at its own heading",
                         r["valid"] and r["road_id"] == test_road_id)
    else:
        print("[SKIP] Test 1 - no self-test point embedded in this database (build with --test-way-id)")

    # Spec test 2: nothing within range -> UNKNOWN, not a fabricated value.
    r = Matcher(db).match(21.5, 106.5, True, 0)
    all_ok &= check("Test 2 - far-away point reports UNKNOWN", not r["valid"] and not r["held"])

    # Spec test 4: way 200 (id 200 in sample_region.osm) is oneway
    # northbound with maxspeed=60. Approaching it heading south (the wrong
    # way) must be disqualified outright, not merely low-confidence.
    way200_mid = (21.0005, 105.8020)
    r_wrong = Matcher(db).match(*way200_mid, True, 180)
    all_ok &= check("Test 4 - oneway road disqualified when approached against its direction", not r_wrong["valid"])
    r_right = Matcher(db).match(*way200_mid, True, 0)
    all_ok &= check("Test 4 - oneway road matches at 60km/h when approached correctly",
                     r_right["valid"] and r_right["speed_limit"] == 60)

    # Spec section 13's worked example: way 300 has different forward(40)/
    # backward(60) limits on the same geometry — heading alone must pick
    # the right one.
    way300_mid = (21.0005, 105.8040)
    r_fwd = Matcher(db).match(*way300_mid, True, 0)
    all_ok &= check("Directional split (spec 13) - forward heading reads 40km/h",
                     r_fwd["valid"] and r_fwd["speed_limit"] == 40)
    r_bwd = Matcher(db).match(*way300_mid, True, 180)
    all_ok &= check("Directional split (spec 13) - backward heading reads 60km/h",
                     r_bwd["valid"] and r_bwd["speed_limit"] == 60)

    print()
    print("NOT covered by this script (need either real hardware or a second,")
    print("purpose-built parallel-roads sample dataset — see the plan):")
    print("  - Test 3: anti-flicker between two close parallel roads")
    print("  - Test 5/6: live GNSS-loss / SD-loss behavior (needs the running firmware)")
    print("  - Test 7/8: hysteresis over a real road-to-road transition, intersections")
    print()
    print("ALL COVERED TESTS " + ("PASSED" if all_ok else "FAILED"))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
