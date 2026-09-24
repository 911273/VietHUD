#!/usr/bin/env python3
"""build_hanoi_fullstreets.py — Rebuild tiles.bin/index.bin/metadata.bin +
names.bin/seg_names.bin for Hà Nội + surroundings from FULL OpenStreetMap
(north_vn.osm), INCLUDING residential/service/small streets, so the vector map
is dense (not just major roads) and speed-limit matching works on real streets.

The shipped tiles.bin came from a WYN source that only has major/classified
roads (nearest road to the user was ~400 m → empty vector). OSM has every
street. build_speedmap.py already keeps all highway types but (a) uses a heavy
full-DOM parse and (b) never emits street names; this script streams the OSM
(bounded memory), filters to the region, and also writes the VNNM names files.

KEEPS the existing WYN cameras.bin / signs.bin untouched (they're position-based
and richer than OSM's). Writes into data/speedmap/, replacing only the road
files. Region-scoped so unique names stay under the uint16 (65535) / 2 MB pool
limits the firmware enforces.

Run: python tools/map_builder/build_hanoi_fullstreets.py
"""
import os, sys, struct, time, zlib, math
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_speedmap as B

ROOT = Path(__file__).resolve().parents[2]
OSM = ROOT / "tools" / "map_builder" / "north_vn.osm"
OUT = ROOT / "data" / "speedmap"

# Hà Nội + lân cận (a bit wider than the raster region so vector has margin)
BBOX = (20.70, 105.50, 21.37, 106.17)  # min_lat, min_lon, max_lat, max_lon
REGION = "VN-HN"
MAPVER = time.strftime("%Y.%m")

NAMES_MAGIC = b"VNNM"
NAMES_VER = 1
MAX_NAMES = 65000        # firmware nameId is uint16 (<65535)
MAX_POOL = 1_950_000     # firmware caps the pool < 2,000,000 bytes

def main():
    minLa, minLo, maxLa, maxLo = BBOX
    print(f"Streaming {OSM.name} ({OSM.stat().st_size/1e6:.0f} MB), region {BBOX} ...")

    nodes = {}            # id -> (latE7, lonE7)  (bbox-filtered)
    segments = []         # dicts like build_speedmap's
    names_list = []       # unique names, index = nameId-1
    name_to_id = {}       # name -> nameId (1-based)
    pool_bytes = 16       # running names.bin pool size (start after header-ish)
    next_id = 1
    nodes_seen = ways_seen = 0

    def name_id(nm):
        nonlocal pool_bytes
        if not nm:
            return 0
        nm = nm.strip()
        if not nm:
            return 0
        if nm in name_to_id:
            return name_to_id[nm]
        if len(name_to_id) >= MAX_NAMES:
            return 0
        enc = nm.encode("utf-8")[:63] + b"\x00"
        if pool_bytes + len(enc) > MAX_POOL:
            return 0
        names_list.append(enc)
        pool_bytes += len(enc)
        nid = len(names_list)  # 1-based
        name_to_id[nm] = nid
        return nid

    context = ET.iterparse(str(OSM), events=("start", "end"))
    _, root = next(context)  # grab the <osm> root so we can prune processed children
    since_prune = 0
    for event, elem in context:
        if event != "end":
            continue
        since_prune += 1
        if since_prune >= 200000:
            root.clear()  # drop already-processed children so memory stays bounded
            since_prune = 0
        tag = elem.tag
        if tag == "node":
            nodes_seen += 1
            try:
                la = float(elem.get("lat")); lo = float(elem.get("lon"))
            except (TypeError, ValueError):
                elem.clear(); continue
            if minLa <= la <= maxLa and minLo <= lo <= maxLo:
                nodes[elem.get("id")] = (B.to_e7(la), B.to_e7(lo))
            elem.clear()
        elif tag == "way":
            ways_seen += 1
            tags = {}
            nds = []
            for ch in elem:
                if ch.tag == "nd":
                    nds.append(ch.get("ref"))
                elif ch.tag == "tag":
                    tags[ch.get("k")] = ch.get("v")
            elem.clear()
            hw = tags.get("highway")
            if not hw or len(nds) < 2:
                continue
            # need at least 2 consecutive in-region nodes
            oneway = tags.get("oneway") in ("yes", "true", "1")
            fwd_tag, bwd_tag, plain = tags.get("maxspeed:forward"), tags.get("maxspeed:backward"), tags.get("maxspeed")
            rc = B.ROAD_CLASS_BY_HIGHWAY.get(hw, 0)
            nid_name = name_id(tags.get("name"))

            for i in range(len(nds) - 1):
                a, b = nds[i], nds[i + 1]
                if a not in nodes or b not in nodes:
                    continue
                la1, lo1 = nodes[a]; la2, lo2 = nodes[b]
                heading = int(round(B.bearing_deg(la1/1e7, lo1/1e7, la2/1e7, lo2/1e7))) % 360
                flags = (B.SEGFLAG_ONEWAY if oneway else 0)

                def emit(direction, spd, src):
                    nonlocal next_id
                    segments.append(dict(id=next_id, s=(la1, lo1), e=(la2, lo2), h=heading, rc=rc,
                                         dir=direction, spd=spd if spd is not None else -1, src=src,
                                         fl=flags, nm=nid_name))
                    next_id += 1

                if fwd_tag is not None or bwd_tag is not None:
                    if fwd_tag is not None: emit(B.DIR_FORWARD, B.parse_maxspeed_kmh(fwd_tag), B.SPEED_SOURCE_OSM_MAXSPEED)
                    if bwd_tag is not None: emit(B.DIR_BACKWARD, B.parse_maxspeed_kmh(bwd_tag), B.SPEED_SOURCE_OSM_MAXSPEED)
                elif plain is not None:
                    emit(B.DIR_FORWARD if oneway else B.DIR_BIDIRECTIONAL, B.parse_maxspeed_kmh(plain), B.SPEED_SOURCE_OSM_MAXSPEED)
                else:
                    d = B.DEFAULT_SPEED_BY_HIGHWAY.get(hw)
                    emit(B.DIR_FORWARD if oneway else B.DIR_BIDIRECTIONAL, d,
                         B.SPEED_SOURCE_DEFAULT if d is not None else B.SPEED_SOURCE_UNKNOWN)

    print(f"  parsed {nodes_seen:,} nodes ({len(nodes):,} in region), {ways_seen:,} ways")
    print(f"  built {len(segments):,} segments, {len(names_list):,} unique names ({pool_bytes:,} B pool)")
    if not segments:
        print("No segments; abort."); sys.exit(1)

    # ---- tiles + index ----
    tiles = {}
    for s in segments:
        tid = B.tile_id(s["s"][0]/1e7, s["s"][1]/1e7)
        tiles.setdefault(tid, []).append(s)

    tiles_blob = bytearray()
    index_entries = []
    max_segs_tile = 0
    for tid in sorted(tiles.keys()):
        segs = tiles[tid]
        if len(segs) > max_segs_tile: max_segs_tile = len(segs)
        off = len(tiles_blob)
        lats = []; lons = []
        for s in segs:
            tiles_blob += struct.pack(B.SEGMENT_FMT, s["id"], s["s"][0], s["s"][1], s["e"][0], s["e"][1],
                                      s["h"], s["rc"], s["dir"], s["spd"], s["src"], s["fl"])
            lats += [s["s"][0], s["e"][0]]; lons += [s["s"][1], s["e"][1]]
        index_entries.append((tid, min(lats), max(lats), min(lons), max(lons), off, len(tiles_blob) - off))

    index_blob = bytearray()
    for (tid, mnLa, mxLa, mnLo, mxLo, off, sz) in index_entries:
        index_blob += struct.pack(B.INDEX_ENTRY_FMT, tid, mnLa, mxLa, mnLo, mxLo, off, sz)
    index_crc = zlib.crc32(bytes(index_blob)) & 0xFFFFFFFF

    (OUT).mkdir(parents=True, exist_ok=True)
    (OUT / "index.bin").write_bytes(index_blob)
    (OUT / "tiles.bin").write_bytes(tiles_blob)

    # self-test point: first segment
    s0 = segments[0]
    metadata = struct.pack(B.METADATA_FMT, B.MAGIC, B.FORMAT_VERSION,
                           REGION.encode("ascii")[:16], MAPVER.encode("ascii")[:16],
                           int(time.time()), len(tiles), len(segments), B.TILE_SIZE_DEG,
                           (s0["s"][0]+s0["e"][0])//2, (s0["s"][1]+s0["e"][1])//2, s0["h"], s0["id"],
                           b"(c) OpenStreetMap contributors, ODbL"[:64], index_crc)
    (OUT / "metadata.bin").write_bytes(metadata)

    # ---- names.bin (VNNM) ----
    offsets = bytearray(); pool = bytearray(); cur = 0
    for enc in names_list:
        offsets += struct.pack("<I", cur); pool += enc; cur += len(enc)
    header = struct.pack("<4sHHII", NAMES_MAGIC, NAMES_VER, len(names_list), len(pool), 0)
    (OUT / "names.bin").write_bytes(header + offsets + pool)

    # ---- seg_names.bin: uint16[segId] (segId 1..N; index 0 unused) ----
    maxid = next_id  # ids are 1..next_id-1
    seg_names = bytearray(maxid * 2)  # zero-filled; [0] unused
    for s in segments:
        struct.pack_into("<H", seg_names, s["id"] * 2, s["nm"])
    (OUT / "seg_names.bin").write_bytes(seg_names)

    print(f"WROTE: tiles.bin {len(tiles_blob)/1e6:.1f}MB ({len(tiles):,} tiles), index.bin ({len(index_entries):,}), "
          f"names.bin ({len(names_list):,}), seg_names.bin ({len(seg_names)/1e6:.1f}MB)")
    print(f"  region={REGION} segments={len(segments):,} crc=0x{index_crc:08X} MAX_SEGS_PER_TILE={max_segs_tile}")
    if max_segs_tile > 2048:
        print(f"  *** WARNING: a tile has {max_segs_tile} segments > firmware kMaxSegmentsPerTile(2048) -> will truncate; bump the firmware constant.")
    print("  (cameras.bin / signs.bin left untouched)")

if __name__ == "__main__":
    main()
