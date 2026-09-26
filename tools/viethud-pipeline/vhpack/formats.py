# -*- coding: utf-8 -*-
"""
vhpack.formats — VietHUD binary output packers (Little-Endian).

Every struct format string here MUST stay byte-identical to
src/map/SpeedMapFormat.h in the firmware. The constants and pack_* helpers are
lifted verbatim from tools/map_builder/build_speedmap.py + build_trafficsigns.py
(which are already proven on-device), plus the names.bin/seg_names.bin **V2**
(uint32) format the firmware reads as of 2026-09-25 (see SdCardManager.cpp and
tools/map_builder/convert_names_v2.py).

Data source is OpenStreetMap (ODbL) + the operator's own surveyed alerts — no
third-party proprietary data.
"""
import struct
import time
import zlib

# ---- speedmap network (SLMP) ----
MAGIC = b"SLMP"
FORMAT_VERSION = 2          # packed tiles.bin (matches SPEEDMAP_FORMAT_VERSION)
TILE_SIZE_DEG = 0.01        # must match SpeedMapFormat.h; firmware reads it from metadata

METADATA_FMT = "<4sH16s16sQIIfiihI64sI"
INDEX_ENTRY_FMT = "<IiiiiII"
SEGMENT_FMT = "<IiiiiHBBhBB"
CAMERA_FMT = "<QiihH"        # id is uint64 (real OSM node ids exceed 2^32)

# ---- signs (VNSG) ----
SIGN_MAGIC = b"VNSG"
SIGN_FORMAT_VERSION = 1
SIGN_HEADER_FMT = "<4sHIHII"   # magic, version, count, reserved, crc32, timestamp
SIGN_POINT_FMT = "<IiiHBBBBH"  # id, latE7, lonE7, dir, type, speed, sub, flags, reserved

# ---- names V2 (VNNM) ----
NAME_MAGIC = b"VNNM"
NAME_FORMAT_VERSION = 2
NAME_HEADER_FMT = "<4sHIIH"    # magic, version=2, count(u32), poolBytes(u32), reserved(u16)

# Enum literals mirrored from SpeedMapFormat.h (kept as plain ints so drift is
# obvious against the header's own literal ordering).
DIR_BIDIRECTIONAL, DIR_FORWARD, DIR_BACKWARD, DIR_UNKNOWN = 0, 1, 2, 3
SEGFLAG_HAS_CONDITIONAL = 0x01
SEGFLAG_ONEWAY = 0x02
(SPEED_SOURCE_UNKNOWN, SPEED_SOURCE_OSM_MAXSPEED, SPEED_SOURCE_OSM_FORWARD,
 SPEED_SOURCE_OSM_BACKWARD, SPEED_SOURCE_DEFAULT, SPEED_SOURCE_CONDITIONAL) = range(6)

# TrafficSignType (SpeedMapFormat.h)
SIGN_TYPE_UNKNOWN = 0
SIGN_TYPE_SPEED_LIMIT = 1
SIGN_TYPE_RESIDENT_AREA = 2
SIGN_TYPE_NO_OVERTAKING = 3
SIGN_TYPE_CAMERA = 4
SIGN_TYPE_TOLL_BOOTH = 5
SIGN_TYPE_TRAFFIC_LIGHT = 6
SIGN_TYPE_DANGER_OTHER = 10

# Fail loudly if any struct drifts from the firmware header.
assert struct.calcsize(METADATA_FMT) == 140, "METADATA_FMT drift vs SpeedMapMetadata"
assert struct.calcsize(INDEX_ENTRY_FMT) == 28, "INDEX_ENTRY_FMT drift vs TileIndexEntry"
assert struct.calcsize(SEGMENT_FMT) == 28, "SEGMENT_FMT drift vs RoadSegment"
assert struct.calcsize(CAMERA_FMT) == 20, "CAMERA_FMT drift vs CameraPoint"
assert struct.calcsize(SIGN_HEADER_FMT) == 20, "SIGN_HEADER_FMT drift vs TrafficSignHeader"
assert struct.calcsize(SIGN_POINT_FMT) == 20, "SIGN_POINT_FMT drift vs TrafficSignPoint"
assert struct.calcsize(NAME_HEADER_FMT) == 16, "NAME_HEADER_FMT drift vs names.bin v2 header"


def to_e7(deg):
    return int(round(deg * 1e7))


def tile_id(lat_deg, lon_deg):
    """Identical to speedmapTileId() in SpeedMapFormat.h."""
    lat_cell = int((lat_deg + 90.0) / TILE_SIZE_DEG)
    lon_cell = int((lon_deg + 180.0) / TILE_SIZE_DEG)
    return ((lat_cell & 0xFFFF) << 16) | (lon_cell & 0xFFFF)


# ---------------------------------------------------------------------------
# Segment / tile / index / metadata (road network)
# ---------------------------------------------------------------------------
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
        cam["speed_limit"] if cam.get("speed_limit") is not None else -1,
        cam["direction"] if cam.get("direction") is not None else 0xFFFF,
    )


def write_network(out_dir, segments, cameras, region, map_version, test_way_id=None):
    """Writes tiles.bin, index.bin, metadata.bin, cameras.bin into out_dir
    (which must already be the .../speedmap directory). Mirrors build_speedmap.py
    build_database() exactly."""
    import os
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(out_dir, "cameras.bin"), "wb") as f:
        for cam in cameras:
            f.write(pack_camera(cam))

    tiles = {}
    for seg in segments:
        tiles.setdefault(tile_id(*seg["start"]), []).append(seg)

    index_entries = b""
    offset = 0
    with open(os.path.join(out_dir, "tiles.bin"), "wb") as tiles_f:
        for tid, segs in sorted(tiles.items()):
            lats = [s["start"][0] for s in segs] + [s["end"][0] for s in segs]
            lons = [s["start"][1] for s in segs] + [s["end"][1] for s in segs]
            tile_bytes = b"".join(pack_segment(s) for s in segs)
            tiles_f.write(tile_bytes)
            index_entries += struct.pack(
                INDEX_ENTRY_FMT, tid,
                to_e7(min(lats)), to_e7(max(lats)),
                to_e7(min(lons)), to_e7(max(lons)),
                offset, len(tile_bytes),
            )
            offset += len(tile_bytes)

    with open(os.path.join(out_dir, "index.bin"), "wb") as f:
        f.write(index_entries)
    index_crc = zlib.crc32(index_entries) & 0xFFFFFFFF

    test_lat_e7 = test_lon_e7 = test_heading = test_road_id = 0
    if test_way_id is not None:
        for s in segments:
            if s.get("way_id") == test_way_id:
                test_lat_e7 = to_e7((s["start"][0] + s["end"][0]) / 2.0)
                test_lon_e7 = to_e7((s["start"][1] + s["end"][1]) / 2.0)
                test_heading = int(round(s["heading"])) % 360
                test_road_id = s["id"]
                break

    attribution = b"(c) OpenStreetMap contributors, ODbL - openstreetmap.org/copyright"
    metadata = struct.pack(
        METADATA_FMT, MAGIC, FORMAT_VERSION,
        region.encode("ascii", "replace")[:16],
        map_version.encode("ascii", "replace")[:16],
        int(time.time()), len(tiles), len(segments), TILE_SIZE_DEG,
        test_lat_e7, test_lon_e7, test_heading, test_road_id,
        attribution[:64], index_crc,
    )
    with open(os.path.join(out_dir, "metadata.bin"), "wb") as f:
        f.write(metadata)
    return dict(tiles=len(tiles), segments=len(segments), cameras=len(cameras),
               index_crc=index_crc)


# ---------------------------------------------------------------------------
# signs.bin (VNSG v1)
# ---------------------------------------------------------------------------
def write_signs(out_dir, signs):
    """signs is a list of dicts: id,lat,lon,direction(0..359 or 0xFFFF),
    sign_type,speed_limit(0..255),sub_type,flags. Writes <out_dir>/signs.bin."""
    import os
    body = b""
    for i, s in enumerate(signs, start=1):
        body += struct.pack(
            SIGN_POINT_FMT,
            s.get("id", i),
            to_e7(s["lat"]), to_e7(s["lon"]),
            s.get("direction", 0xFFFF) if s.get("direction") is not None else 0xFFFF,
            s["sign_type"] & 0xFF,
            max(0, min(255, int(s.get("speed_limit") or 0))),
            s.get("sub_type", 0) & 0xFF,
            s.get("flags", 0) & 0xFF,
            0,
        )
    crc = zlib.crc32(body) & 0xFFFFFFFF
    header = struct.pack(SIGN_HEADER_FMT, SIGN_MAGIC, SIGN_FORMAT_VERSION,
                         len(signs), 0, crc, int(time.time()))
    with open(os.path.join(out_dir, "signs.bin"), "wb") as f:
        f.write(header)
        f.write(body)
    return len(signs)


# ---------------------------------------------------------------------------
# names.bin + seg_names.bin (V2, uint32)
# ---------------------------------------------------------------------------
def write_names_v2(out_dir, seg_name_by_id, max_seg_id):
    """
    seg_name_by_id: dict {segId(1-based int) -> street-name str (already
                    abbreviated/normalized as desired)}. Empty/None -> no name.
    max_seg_id:     highest segId in the network (seg_names.bin is sized to it).

    Writes:
      names.bin      v2: <4sHIIH> + offsets[count](u32) + UTF-8 NUL-terminated pool
      seg_names.bin  flat u32 array, index = segId (1-based; index 0 unused)
    Returns (unique_name_count, pool_bytes).
    """
    import os
    # Deduplicate names -> 1-based ids, preserving first-seen order.
    name_to_id = {}
    ordered = []
    for name in seg_name_by_id.values():
        if not name:
            continue
        if name not in name_to_id:
            name_to_id[name] = len(ordered) + 1  # 1-based
            ordered.append(name)

    # Build the pool + offsets.
    pool = bytearray()
    offsets = []
    for name in ordered:
        offsets.append(len(pool))
        pool += name.encode("utf-8")
        pool.append(0)  # NUL terminator

    header = struct.pack(NAME_HEADER_FMT, NAME_MAGIC, NAME_FORMAT_VERSION,
                         len(ordered), len(pool), 0)
    with open(os.path.join(out_dir, "names.bin"), "wb") as f:
        f.write(header)
        f.write(struct.pack("<%dI" % len(offsets), *offsets) if offsets else b"")
        f.write(bytes(pool))

    # seg_names.bin: flat u32, index by segId (1-based). Size to max_seg_id+1.
    seg_ids = [0] * (max_seg_id + 1)
    for seg_id, name in seg_name_by_id.items():
        if name and 0 < seg_id <= max_seg_id:
            seg_ids[seg_id] = name_to_id.get(name, 0)
    with open(os.path.join(out_dir, "seg_names.bin"), "wb") as f:
        f.write(struct.pack("<%dI" % len(seg_ids), *seg_ids))

    return len(ordered), len(pool)
