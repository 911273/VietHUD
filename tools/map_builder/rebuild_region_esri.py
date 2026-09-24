#!/usr/bin/env python3
"""rebuild_region_esri.py — Replace the watermarked CartoDB tiles over Hà Nội +
surroundings with CLEAN Esri "Dark Gray Base" tiles (free, no API key, dark
theme that matches this HUD).

Why: CartoDB keyless tiles now carry an "API KEY REQUIRED" watermark, and
Wikimedia rate-limited a bulk pull. Esri's ArcGIS Online basemaps are free for
moderate use and tolerate parallel downloads. Tiles are re-encoded to small
JPEGs (<=~9 KB) so they fit the firmware's 12 KB tile buffer.

Esri tile URL order is /{z}/{y}/{x} (row before col). Merges with NEW tiles
OVERRIDING existing keys; streaming write; validated before replace.
Run: python tools/map_builder/rebuild_region_esri.py
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
CACHE = ROOT / "tools" / "map_builder" / "cache" / "esri_darkgray"
CACHE.mkdir(parents=True, exist_ok=True)

REGION = (20.75, 105.55, 21.32, 106.12)  # Hà Nội + lân cận
ZOOMS = [13, 14, 15, 16]
URL = "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}"
UA = "VietHUD/1.0 (personal offline driving HUD; contact vupq@epu.edu.vn)"
THREADS = 24
JPEG_Q = 72          # keep tiles small; device buffer is 12288 bytes
MAX_BYTES = 11800    # hard cap so nothing exceeds the device tile buffer
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

def encode(png_or_jpg_bytes):
    img = Image.open(io.BytesIO(png_or_jpg_bytes)).convert("RGB")
    q = JPEG_Q
    while q >= 40:
        buf = io.BytesIO(); img.save(buf, format="JPEG", quality=q, optimize=True)
        b = buf.getvalue()
        if len(b) <= MAX_BYTES:
            return b
        q -= 8
    return b  # last (smallest) attempt

def fetch(z, x, y, session):
    cf = CACHE / f"esri_{z}_{x}_{y}.jpg"
    if cf.exists():
        b = cf.read_bytes()
        if 100 < len(b) <= MAX_BYTES:
            return (key(z,x,y), b)
    url = URL.format(z=z, y=y, x=x)
    for _ in range(4):
        try:
            r = session.get(url, timeout=25, headers={"User-Agent": UA})
            if r.status_code == 200 and len(r.content) > 200:
                jb = encode(r.content)
                cf.write_bytes(jb)
                return (key(z,x,y), jb)
            if r.status_code in (404, 204):
                return None
        except Exception:
            time.sleep(0.4)
    return None

def main():
    if not MAPTILES.exists():
        print("ERROR: maptiles.bin missing"); sys.exit(1)
    tiles = region_tiles()
    print(f"Region {REGION} zooms {ZOOMS}: {len(tiles):,} Esri dark-gray tiles")
    if len(tiles) > MAX_TILES:
        print("ERROR exceeds ceiling"); sys.exit(1)

    new = {}
    s = requests.Session(); t0 = time.time(); done = 0; tot = len(tiles); fails = 0
    with ThreadPoolExecutor(max_workers=THREADS) as ex:
        futs = {ex.submit(fetch, z, x, y, s): (z,x,y) for (z,x,y) in tiles}
        for fu in as_completed(futs):
            r = fu.result(); done += 1
            if r: new[r[0]] = r[1]
            else: fails += 1
            if done % 500 == 0 or done == tot:
                el = time.time()-t0; rate = done/el if el else 0
                print(f"  {done:,}/{tot:,} ({done/tot*100:.0f}%) {rate:.0f}/s ETA {(tot-done)/max(rate,1)/60:.1f}m ok={len(new):,} fail={fails}")
    print(f"Got {len(new):,} tiles ({fails} failed) in {time.time()-t0:.0f}s")
    if len(new) < tot * 0.5:
        print(f"WARNING: only {len(new)}/{tot} downloaded — proceeding to merge what we have.")
    if not new:
        print("Nothing downloaded; abort."); sys.exit(1)

    with open(MAPTILES, "rb") as f:
        magic, ver, minz, maxz, total, ioff, isz, _ = struct.unpack(HDR_FMT, f.read(64))
        assert magic == b"VNMB"
        idx = f.read(isz)
        old = [struct.unpack(ENT_FMT, idx[i*16:(i+1)*16]) for i in range(total)]
    combined = {}
    for (k, off, sz, rsv) in old:
        combined[k] = ("old", off, sz)
    replaced = 0
    for k, v in new.items():
        if k in combined: replaced += 1
        combined[k] = ("new", v)
    keys = sorted(combined.keys()); tc = len(keys)
    allz = [(k >> 40) & 0xFFFFFF for k in keys]; nmin, nmax = min(allz), max(allz)
    print(f"  merged {tc:,} tiles ({replaced:,} replaced, {len(new)-replaced:,} added)")

    isz2 = tc*16; blob0 = 64+isz2; entries = bytearray(); cur = blob0
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
    szt = tmp.stat().st_size
    with open(tmp, "rb") as f:
        m, vv, mnz, mxz, tcc, io2, is2, _ = struct.unpack(HDR_FMT, f.read(64))
        assert m == b"VNMB" and tcc == tc, "hdr mismatch"
        ib = f.read(is2); prev = -1; big = 0
        for i in range(tc):
            kk, oo, ss, _ = struct.unpack(ENT_FMT, ib[i*16:(i+1)*16])
            assert kk > prev and oo+ss <= szt, "index bad"; prev = kk
            if ss > 12288: big += 1
        print(f"  (tiles over 12288B device buffer: {big})")
    os.replace(tmp, MAPTILES)
    print(f"SUCCESS: maptiles.bin {tc:,} tiles zoom {nmin}..{nmax} {szt/1e6:.1f}MB — Hà Nội now clean Esri dark-gray")

if __name__ == "__main__":
    main()
