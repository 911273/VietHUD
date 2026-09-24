#!/usr/bin/env python3
"""rebuild_region_wikimedia.py — Replace the watermarked CartoDB tiles over
Hà Nội + surroundings with CLEAN Wikimedia OSM tiles (maps.wikimedia.org).

CartoDB's keyless basemaps now stamp "API KEY REQUIRED" on every tile, so the
shipped maptiles.bin raster is watermarked/faint. Wikimedia serves clean,
labelled OSM tiles with no key. This downloads the region at z13..z16 (full
bbox, gap-free) and MERGES with NEW tiles OVERRIDING the existing watermarked
ones (same z/x/y key), keeping the rest of the country untouched.

Streaming merge (old blobs copied by offset), validated before replace.
Run: python tools/map_builder/rebuild_region_wikimedia.py
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
CACHE = ROOT / "tools" / "map_builder" / "cache" / "wikimedia"
CACHE.mkdir(parents=True, exist_ok=True)

REGION = (20.75, 105.55, 21.32, 106.12)  # Hà Nội + lân cận
ZOOMS = [13, 14, 15, 16]
URL = "https://maps.wikimedia.org/osm-intl/{z}/{x}/{y}.png"
UA = "VietHUD/1.0 (personal offline driving HUD; contact vupq@epu.edu.vn)"
THREADS = 12  # polite to Wikimedia
MAX_TILES = 40000

HDR_FMT = "<4sHBBIII44s"; assert struct.calcsize(HDR_FMT) == 64
ENT_FMT = "<QIHH";        assert struct.calcsize(ENT_FMT) == 16

def deg2num(lat, lon, z):
    n = 2 ** z
    return int((lon+180)/360*n), int((1-math.asinh(math.tan(math.radians(lat)))/math.pi)/2*n)

def key(z, x, y):
    return (z << 40) | (x << 20) | y

def region_tiles():
    minLa, minLo, maxLa, maxLo = REGION
    out = []
    for z in ZOOMS:
        x0, y1 = deg2num(maxLa, minLo, z)
        x1, y0 = deg2num(minLa, maxLo, z)
        for x in range(min(x0,x1), max(x0,x1)+1):
            for y in range(min(y0,y1), max(y0,y1)+1):
                out.append((z, x, y))
    return out

def fetch(z, x, y, session):
    cf = CACHE / f"wm_{z}_{x}_{y}.jpg"
    if cf.exists():
        b = cf.read_bytes()
        if len(b) > 100:
            return (key(z,x,y), b)
    url = URL.format(z=z, x=x, y=y)
    for _ in range(3):
        try:
            r = session.get(url, timeout=25, headers={"User-Agent": UA})
            if r.status_code == 200 and len(r.content) > 200:
                img = Image.open(io.BytesIO(r.content)).convert("RGB")
                buf = io.BytesIO(); img.save(buf, format="JPEG", quality=78, optimize=True)
                jb = buf.getvalue(); cf.write_bytes(jb)
                return (key(z,x,y), jb)
            if r.status_code in (404, 204):
                return None
        except Exception:
            time.sleep(0.5)
    return None

def main():
    if not MAPTILES.exists():
        print("ERROR: maptiles.bin missing"); sys.exit(1)
    tiles = region_tiles()
    print(f"Region {REGION} zooms {ZOOMS}: {len(tiles):,} tiles from Wikimedia")
    if len(tiles) > MAX_TILES:
        print(f"ERROR exceeds ceiling {MAX_TILES}"); sys.exit(1)

    new = {}
    s = requests.Session(); t0 = time.time(); done = 0; tot = len(tiles)
    with ThreadPoolExecutor(max_workers=THREADS) as ex:
        futs = {ex.submit(fetch, z, x, y, s): (z,x,y) for (z,x,y) in tiles}
        for fu in as_completed(futs):
            r = fu.result(); done += 1
            if r: new[r[0]] = r[1]
            if done % 500 == 0 or done == tot:
                el = time.time()-t0; rate = done/el if el else 0
                print(f"  {done:,}/{tot:,} ({done/tot*100:.0f}%) {rate:.0f}/s ETA {(tot-done)/max(rate,1)/60:.1f}m ok={len(new):,}")
    print(f"Got {len(new):,} clean tiles in {time.time()-t0:.0f}s")
    if not new:
        print("Nothing downloaded; abort (no merge)."); sys.exit(1)

    # merge: NEW overrides existing keys
    with open(MAPTILES, "rb") as f:
        magic, ver, minz, maxz, total, ioff, isz, _ = struct.unpack(HDR_FMT, f.read(64))
        assert magic == b"VNMB"
        idx = f.read(isz)
        old = [struct.unpack(ENT_FMT, idx[i*16:(i+1)*16]) for i in range(total)]  # (k,off,sz,rsv)
    combined = {}
    for (k, off, sz, rsv) in old:
        combined[k] = ("old", off, sz)
    replaced = 0
    for k, v in new.items():
        if k in combined: replaced += 1
        combined[k] = ("new", v)
    keys = sorted(combined.keys())
    tc = len(keys)
    allz = [(k >> 40) & 0xFFFFFF for k in keys]
    nmin, nmax = min(allz), max(allz)
    print(f"  merged: {tc:,} tiles ({replaced:,} watermarked tiles replaced, {len(new)-replaced:,} added)")

    isz2 = tc * 16; blob0 = 64 + isz2
    entries = bytearray(); cur = blob0
    for k in keys:
        src = combined[k]; sz = src[2] if src[0]=="old" else len(src[1])
        entries += struct.pack(ENT_FMT, k, cur, sz, 0); cur += sz
    header = struct.pack(HDR_FMT, b"VNMB", 1, nmin, nmax, tc, 64, isz2, b"\x00"*44)
    tmp = MAPTILES.with_suffix(".bin.tmp")
    with open(MAPTILES, "rb") as fin, open(tmp, "wb") as fout:
        fout.write(header); fout.write(entries)
        for k in keys:
            src = combined[k]
            if src[0] == "old":
                fin.seek(src[1]); fout.write(fin.read(src[2]))
            else:
                fout.write(src[1])
    # validate before replace
    szt = tmp.stat().st_size
    with open(tmp, "rb") as f:
        m, vv, mnz, mxz, tcc, io2, is2, _ = struct.unpack(HDR_FMT, f.read(64))
        assert m == b"VNMB" and tcc == tc, "header mismatch"
        ib = f.read(is2); prev = -1
        for i in range(tc):
            kk, oo, ss, _ = struct.unpack(ENT_FMT, ib[i*16:(i+1)*16])
            assert kk > prev and oo+ss <= szt, "index bad"; prev = kk
    os.replace(tmp, MAPTILES)
    print(f"SUCCESS: maptiles.bin {tc:,} tiles zoom {nmin}..{nmax} {szt/1e6:.1f}MB (Hà Nội now clean Wikimedia)")

if __name__ == "__main__":
    main()
