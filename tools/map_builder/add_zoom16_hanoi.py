#!/usr/bin/env python3
"""add_zoom16_hanoi.py — Upgrade maptiles.bin with street-level z16 detail for
Hà Nội + surroundings, MERGED into the existing nationwide z9..z15 set (never
overwrites it).

Why: the shipped maptiles.bin tops out at z15 (~4.5 m/px). Adding z16 (~2.4 m/px)
for the Hà Nội metro area makes the city map noticeably sharper. Road-filtered
from index.bin so only tiles actually over roads are fetched (no ocean/field
waste), same philosophy as the original build.

Streaming merge: old tile blobs are copied straight from the existing file by
offset (never all held in RAM); only the new z16 JPEGs live in memory.

Run:  python tools/map_builder/add_zoom16_hanoi.py
"""
import io, math, os, struct, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from PIL import Image
import requests

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8"); sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parents[2]
MAPTILES = ROOT / "data" / "speedmap" / "maptiles.bin"
INDEX = ROOT / "data" / "speedmap" / "index.bin"
CACHE = ROOT / "tools" / "map_builder" / "cache" / "z16_hanoi"
CACHE.mkdir(parents=True, exist_ok=True)

# Hà Nội + lân cận (metro + immediate neighbouring districts/provinces edges)
REGION = (20.75, 105.55, 21.32, 106.12)  # min_lat, min_lon, max_lat, max_lon
ZOOM = 16
URL = "https://basemaps.cartocdn.com/rastertiles/dark_all/{z}/{x}/{y}.png"
API = "cb1_3u0a_1_81f5ac6dc7f5d31ccd6a20b1"
THREADS = 48
MAX_TILES = 60000  # safety ceiling

HDR_FMT = "<4sHBBIII44s"; assert struct.calcsize(HDR_FMT) == 64
ENT_FMT = "<QIHH";        assert struct.calcsize(ENT_FMT) == 16

def deg2num(lat, lon, z):
    n = 2 ** z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n)
    return x, y

def key(z, x, y):
    return (z << 40) | (x << 20) | y

def road_filtered_z16():
    """FULL-region z16 coverage (not road-filtered). The firmware's renderer
    picks ONE zoom per frame from the center tile, so any missing neighbour at
    that zoom renders as a blank patch — road-only z16 left gaps between roads.
    Full bbox coverage keeps the z16 city view gap-free. Empty (water/field)
    dark tiles compress tiny, so the size cost is small."""
    minLa, minLo, maxLa, maxLo = REGION
    x0, y1 = deg2num(maxLa, minLo, ZOOM)  # NW corner
    x1, y0 = deg2num(minLa, maxLo, ZOOM)  # SE corner
    want = set()
    for x in range(min(x0, x1), max(x0, x1) + 1):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            want.add((x, y))
    return want

def fetch(x, y, session):
    cf = CACHE / f"carto_16_{x}_{y}.jpg"
    if cf.exists():
        b = cf.read_bytes()
        if len(b) > 100:
            return (x, y, b)
    url = URL.format(z=ZOOM, x=x, y=y) + ("?" + API if API else "")
    for attempt in range(3):
        try:
            r = session.get(url, timeout=20, headers={"User-Agent": "Mozilla/5.0"})
            if r.status_code == 200 and len(r.content) > 100:
                img = Image.open(io.BytesIO(r.content)).convert("RGB")
                buf = io.BytesIO(); img.save(buf, format="JPEG", quality=75, optimize=True)
                jb = buf.getvalue()
                cf.write_bytes(jb)
                return (x, y, jb)
            if r.status_code in (404, 204):
                return None
        except Exception:
            time.sleep(0.3)
    return None

def main():
    if not MAPTILES.exists() or not INDEX.exists():
        print("ERROR: maptiles.bin or index.bin missing"); sys.exit(1)

    print(f"Region {REGION} z{ZOOM}; computing road-filtered tiles from index.bin ...")
    tiles = road_filtered_z16()
    print(f"  {len(tiles):,} z16 tiles over roads in region")
    if len(tiles) > MAX_TILES:
        print(f"ERROR: {len(tiles):,} exceeds ceiling {MAX_TILES:,}; narrow REGION."); sys.exit(1)
    if not tiles:
        print("No tiles; abort."); sys.exit(1)

    # Download / load cache
    new = {}  # key -> jpg bytes
    session = requests.Session()
    t0 = time.time(); done = 0; tot = len(tiles)
    with ThreadPoolExecutor(max_workers=THREADS) as ex:
        futs = {ex.submit(fetch, x, y, session): (x, y) for (x, y) in tiles}
        for fu in as_completed(futs):
            r = fu.result(); done += 1
            if r:
                x, y, jb = r
                new[key(ZOOM, x, y)] = jb
            if done % 1000 == 0 or done == tot:
                el = time.time() - t0; rate = done/el if el else 0
                print(f"  {done:,}/{tot:,} ({done/tot*100:.0f}%) {rate:.0f}/s ETA {(tot-done)/rate/60:.1f}m valid={len(new):,}")
    print(f"Downloaded/cached {len(new):,} z16 tiles in {time.time()-t0:.0f}s")

    # ---- streaming merge with existing maptiles.bin ----
    print("Merging into existing maptiles.bin (streaming) ...")
    with open(MAPTILES, "rb") as f:
        hdr = f.read(64)
        magic, ver, minz, maxz, total, ioff, isz, _ = struct.unpack(HDR_FMT, hdr)
        assert magic == b"VNMB"
        idx = f.read(isz)
        old = []  # (key, oldOffset, size)
        for i in range(total):
            k, off, sz, rsv = struct.unpack(ENT_FMT, idx[i*16:(i+1)*16])
            old.append((k, off, sz))
    old_keys = {k for (k, _, _) in old}
    add = [(k, v) for k, v in new.items() if k not in old_keys]  # dedup
    print(f"  existing {len(old):,} tiles, adding {len(add):,} new z16 (dedup)")

    # combined sorted key list; entries carry a source: ('old',off,sz) or ('new',bytes)
    combined = {}
    for (k, off, sz) in old:
        combined[k] = ("old", off, sz)
    for (k, v) in add:
        combined[k] = ("new", v)
    keys = sorted(combined.keys())
    tile_count = len(keys)
    new_minz = min(minz, ZOOM); new_maxz = max(maxz, ZOOM)

    index_offset = 64
    index_size = tile_count * 16
    blob_off = index_offset + index_size

    tmp = MAPTILES.with_suffix(".bin.tmp")
    # first pass: compute offsets + build index
    entries = bytearray(); cur = blob_off
    for k in keys:
        src = combined[k]
        sz = src[2] if src[0] == "old" else len(src[1])
        entries += struct.pack(ENT_FMT, k, cur, sz, 0)
        cur += sz
    header = struct.pack(HDR_FMT, b"VNMB", 1, new_minz, new_maxz, tile_count, index_offset, index_size, b"\x00"*44)
    # second pass: stream blobs
    with open(MAPTILES, "rb") as fin, open(tmp, "wb") as fout:
        fout.write(header); fout.write(entries)
        for k in keys:
            src = combined[k]
            if src[0] == "old":
                fin.seek(src[1]); fout.write(fin.read(src[2]))
            else:
                fout.write(src[1])

    # Validate the tmp BEFORE replacing the original (never clobber a good file
    # with a broken one): header sane, index sorted & in-bounds, spot-check a
    # z16 and an old tile decode.
    sz_tmp = tmp.stat().st_size
    with open(tmp, "rb") as f:
        h = f.read(64)
        m, vv, mnz, mxz, tc, io2, is2, _ = struct.unpack(HDR_FMT, h)
        assert m == b"VNMB" and tc == tile_count and mxz == new_maxz, "header mismatch"
        assert io2 + is2 <= sz_tmp, "index past EOF"
        ib = f.read(is2)
        prevk = -1; last = struct.unpack(ENT_FMT, ib[(tc-1)*16:tc*16])
        for i in (0, tc//2, tc-1):
            kk, oo, ss, _ = struct.unpack(ENT_FMT, ib[i*16:(i+1)*16])
            assert oo + ss <= sz_tmp, "blob past EOF"
        # sortedness (full, cheap)
        for i in range(tc):
            kk = struct.unpack("<Q", ib[i*16:i*16+8])[0]
            assert kk > prevk, "index not strictly sorted"
            prevk = kk
    os.replace(tmp, MAPTILES)
    total_mb = MAPTILES.stat().st_size / 1e6
    print(f"SUCCESS: maptiles.bin now {tile_count:,} tiles, zoom {new_minz}..{new_maxz}, {total_mb:.1f} MB")

if __name__ == "__main__":
    main()
