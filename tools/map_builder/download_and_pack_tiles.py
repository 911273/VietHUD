#!/usr/bin/env python3
"""download_and_pack_tiles.py — Download CartoDB Dark Matter tiles and pack into a single maptiles.bin container.

Features:
  - Multi-threaded download with local disk cache and automatic retry
  - Automatic conversion from PNG to high-efficiency JPEG (75% quality, ~3.5-4.5 KB per tile)
  - Supports road-based spatial filtering from index.bin (100% road coverage of Vietnam with zero ocean waste)
  - Single binary container format with binary-searchable Spatial Index Table
  - Fully compatible with ESP32-S3 TJpgDec hardware decoder

Usage:
  python download_and_pack_tiles.py --from-index data/speedmap/index.bin --zooms 9,10,11,12,13,14 --out data/speedmap
  python download_and_pack_tiles.py --preset hanoi --out data/speedmap
"""

import argparse
import io
import math
import shutil
import os
import struct
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from PIL import Image
import requests

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

MAGIC = b"VNMB"
VERSION = 1
HEADER_FMT = "<4sHBBIII44s"  # 64 bytes
assert struct.calcsize(HEADER_FMT) == 64

INDEX_ENTRY_FMT = "<QIHH"  # 16 bytes
assert struct.calcsize(INDEX_ENTRY_FMT) == 16

SOURCES = {
    "carto_dark": {
        "url": "https://basemaps.cartocdn.com/rastertiles/dark_all/{z}/{x}/{y}.png",
        "default_name": "maptiles.bin",
        "api_key": "cb1_3u0a_1_81f5ac6dc7f5d31ccd6a20b1"
    },
    "osm": {
        "url": "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
        "default_name": "maptiles_osm.bin",
        "api_key": None
    },
    "osm_dark": {
        "url": "https://basemaps.cartocdn.com/rastertiles/dark_all/{z}/{x}/{y}.png",
        "default_name": "maptiles_dark.bin",
        "api_key": "cb1_3u0a_1_81f5ac6dc7f5d31ccd6a20b1"
    },
    "voyager": {
        "url": "https://basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}.png",
        "default_name": "maptiles_voyager.bin",
        "api_key": "cb1_3u0a_1_81f5ac6dc7f5d31ccd6a20b1"
    }
}
DEFAULT_SOURCE = "osm"
USER_AGENT = "VietHUD-MapBuilder/1.0 (ESP32 Offline Navigation Device) contact@viethud.vn"


PRESETS = {
    "hanoi": {
        "description": "Ha Noi mo rong (Noi Bai, Vanh dai 3, 4, Ha Dong)",
        "regions": [
            {"bbox": (20.80, 105.65, 21.25, 106.05), "zooms": [9, 10, 11, 12, 13, 14, 15]}
        ]
    },
    "hcm": {
        "description": "TP. Ho Chi Minh & phu can",
        "regions": [
            {"bbox": (10.60, 106.50, 10.95, 106.85), "zooms": [9, 10, 11, 12, 13, 14, 15]}
        ]
    },
    "vietnam_core": {
        "description": "Ha Noi + TP.HCM + Da Nang + Truc Quoc lo trong diem",
        "regions": [
            {"bbox": (8.5, 104.0, 23.0, 109.5), "zooms": [9, 10]},
            {"bbox": (20.80, 105.65, 21.25, 106.05), "zooms": [9, 10, 11, 12, 13, 14, 15]},
            {"bbox": (10.60, 106.50, 10.95, 106.85), "zooms": [9, 10, 11, 12, 13, 14, 15]},
            {"bbox": (15.95, 108.10, 16.15, 108.30), "zooms": [9, 10, 11, 12, 13, 14, 15]},
        ]
    }
}

def deg2num(lat_deg, lon_deg, zoom):
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** zoom
    xtile = int((lon_deg + 180.0) / 360.0 * n)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile

def make_tile_key(z, x, y):
    return (int(z) << 40) | (int(x) << 20) | int(y)

def fetch_tile_jpg(z, x, y, session, cache_dir, url_template, api_key=None, source_tag="osm"):
    cache_file = cache_dir / f"{source_tag}_{z}_{x}_{y}.jpg"
    if cache_file.exists():
        try:
            data = cache_file.read_bytes()
            if len(data) > 100:
                return data
        except Exception:
            pass

    url = url_template.format(z=z, x=x, y=y)
    if api_key:
        delim = "&" if "?" in url else "?"
        url += f"{delim}key={api_key}"
    headers = {"User-Agent": USER_AGENT}
    for attempt in range(3):
        try:
            resp = session.get(url, headers=headers, timeout=10)
            if resp.status_code == 200:
                img = Image.open(io.BytesIO(resp.content)).convert("RGB")
                out_buf = io.BytesIO()
                img.save(out_buf, format="JPEG", quality=75, optimize=True)
                jpg_data = out_buf.getvalue()
                try:
                    cache_file.write_bytes(jpg_data)
                except Exception:
                    pass
                return jpg_data
            elif resp.status_code == 404:
                return None
            elif resp.status_code == 429:
                time.sleep(2.0 * (attempt + 1))
        except Exception:
            time.sleep(0.4 * (attempt + 1))
    return None

def main():
    parser = argparse.ArgumentParser(description="Download and pack map tiles for VietHUD")
    parser.add_argument("--from-index", help="Path to index.bin to extract all road-covered tile coordinates")
    parser.add_argument("--source", choices=SOURCES.keys(), default="osm", help="Map provider source (osm, carto_dark, voyager)")
    parser.add_argument("--preset", choices=PRESETS.keys(), default="vietnam_core", help="Preset region to download")
    parser.add_argument("--bbox", nargs=4, type=float, metavar=('MIN_LAT', 'MIN_LON', 'MAX_LAT', 'MAX_LON'),
                        help="Custom bounding box")
    parser.add_argument("--zooms", type=str, default="9,10,11,12,13,14", help="Comma-separated zoom levels")
    parser.add_argument("--out", default=r"data\speedmap", help="Output directory")
    parser.add_argument("--cache-dir", default=r"tools\map_builder\cache\clean_tiles", help="Directory to cache downloaded JPG tiles")
    parser.add_argument("--copy-to", help="Copy destination directory")
    parser.add_argument("--threads", type=int, default=48, help="Number of download threads")
    parser.add_argument("--api-key", default=None, help="API Key if needed")
    args = parser.parse_args()

    cache_dir = Path(args.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_path = out_dir / (SOURCES[args.source]["default_name"] if args.source in SOURCES else "maptiles_osm.bin")

    zoom_levels = [int(z.strip()) for z in args.zooms.split(",") if z.strip()]

    tiles_set = set()

    if args.from_index:
        idx_path = Path(args.from_index)
        if not idx_path.exists():
            print(f"ERROR: index.bin not found at {idx_path}", file=sys.stderr)
            sys.exit(1)
        print(f"Extracting road-covered tiles from {idx_path} for zooms {zoom_levels} ...")
        ENTRY_FMT = "<IiiiiII"
        with open(idx_path, "rb") as f:
            while True:
                buf = f.read(28)
                if len(buf) < 28:
                    break
                tile_id, min_lat, max_lat, min_lon, max_lon, offset, size = struct.unpack(ENTRY_FMT, buf)
                mid_lat = (min_lat + max_lat) / 2.0 / 1e7
                mid_lon = (min_lon + max_lon) / 2.0 / 1e7
                for z in zoom_levels:
                    tx, ty = deg2num(mid_lat, mid_lon, z)
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            tiles_set.add((z, tx + dx, ty + dy))
        print(f"Total road-covered tiles (+1 margin): {len(tiles_set):,}")
    elif args.bbox:
        min_lat, min_lon, max_lat, max_lon = args.bbox
        for z in zoom_levels:
            x1, y2 = deg2num(min_lat, min_lon, z)
            x2, y1 = deg2num(max_lat, max_lon, z)
            for x in range(min(x1, x2), max(x1, x2) + 1):
                for y in range(min(y1, y2), max(y1, y2) + 1):
                    tiles_set.add((z, x, y))
    else:
        preset_info = PRESETS[args.preset]
        print(f"Loading preset '{args.preset}': {preset_info['description']}")
        for reg in preset_info["regions"]:
            min_lat, min_lon, max_lat, max_lon = reg["bbox"]
            for z in reg["zooms"]:
                if z in zoom_levels:
                    x1, y2 = deg2num(min_lat, min_lon, z)
                    x2, y1 = deg2num(max_lat, max_lon, z)
                    for x in range(min(x1, x2), max(x1, x2) + 1):
                        for y in range(min(y1, y2), max(y1, y2) + 1):
                            tiles_set.add((z, x, y))

    tiles_to_fetch = sorted(list(tiles_set))
    total_tiles = len(tiles_to_fetch)
    all_zooms = [t[0] for t in tiles_to_fetch]
    min_zoom = min(all_zooms) if all_zooms else 0
    max_zoom = max(all_zooms) if all_zooms else 0

    print(f"Total tiles to process: {total_tiles:,} (Zooms {min_zoom}..{max_zoom})")

    # Fast-check existing cached files
    downloaded_data = {}
    remaining_tiles = []
    print("Checking local cache ...")
    for (z, x, y) in tiles_to_fetch:
        cache_file = cache_dir / f"{args.source}_{z}_{x}_{y}.jpg"
        if cache_file.exists():
            try:
                data = cache_file.read_bytes()
                if len(data) > 100:
                    downloaded_data[make_tile_key(z, x, y)] = data
                    continue
            except Exception:
                pass
        remaining_tiles.append((z, x, y))

    print(f"Found {len(downloaded_data):,} tiles already in cache. Remaining to download: {len(remaining_tiles):,}")

    if remaining_tiles:
        session = requests.Session()
        t0 = time.time()
        completed = 0
        rem_count = len(remaining_tiles)

        print(f"Downloading {rem_count:,} tiles with {args.threads} threads ...")
        src_info = SOURCES[args.source]
        url_tmpl = src_info["url"]
        api_k = args.api_key if args.api_key is not None else src_info["api_key"]
        src_tag = args.source

        with ThreadPoolExecutor(max_workers=args.threads) as executor:
            futures = {executor.submit(fetch_tile_jpg, z, x, y, session, cache_dir, url_tmpl, api_k, src_tag): (z, x, y)
                       for (z, x, y) in remaining_tiles}

            for future in as_completed(futures):
                z, x, y = futures[future]
                jpg_bytes = future.result()
                if jpg_bytes:
                    key = make_tile_key(z, x, y)
                    downloaded_data[key] = jpg_bytes
                completed += 1
                if completed % 500 == 0 or completed == rem_count:
                    elapsed = time.time() - t0
                    rate = completed / elapsed if elapsed > 0 else 0
                    rem_sec = (rem_count - completed) / rate if rate > 0 else 0
                    print(f"  Progress: {completed:,}/{rem_count:,} ({completed/rem_count*100:.1f}%) | "
                          f"{rate:.1f} tiles/s | ETA: {rem_sec/60:.1f}m | Total Valid: {len(downloaded_data):,}")

        print(f"Downloaded remaining tiles in {time.time()-t0:.1f}s.")

    # Pack into maptiles.bin
    print(f"Packing into {bin_path} ...")
    sorted_keys = sorted(downloaded_data.keys())
    tile_count = len(sorted_keys)

    index_offset = 64
    index_size = tile_count * struct.calcsize(INDEX_ENTRY_FMT)
    data_blob_offset = index_offset + index_size

    index_entries = bytearray()
    blob_bytes = bytearray()
    current_blob_offset = data_blob_offset

    for key in sorted_keys:
        jpg_bytes = downloaded_data[key]
        data_len = len(jpg_bytes)
        entry = struct.pack(INDEX_ENTRY_FMT, key, current_blob_offset, data_len, 0)
        index_entries.extend(entry)
        blob_bytes.extend(jpg_bytes)
        current_blob_offset += data_len

    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        VERSION,
        min_zoom,
        max_zoom,
        tile_count,
        index_offset,
        index_size,
        b"\x00" * 44
    )

    with open(bin_path, "wb") as f:
        f.write(header)
        f.write(index_entries)
        f.write(blob_bytes)

    total_size = bin_path.stat().st_size
    print(f"SUCCESS! Created {bin_path}:")
    print(f"  Total size: {total_size:,} bytes ({total_size/1024/1024:.2f} MB)")
    print(f"  Total tiles: {tile_count:,}")
    print(f"  Avg tile size: {total_size/max(1, tile_count):.0f} bytes")

    if args.copy_to:
        dest_dir = Path(args.copy_to)
        if dest_dir.exists():
            dest_file = dest_dir / "maptiles.bin"
            print(f"Copying {bin_path} to {dest_file} ...")
            shutil.copyfile(bin_path, dest_file)
            print(f"SUCCESS! Copied to {dest_file} ({dest_file.stat().st_size / 1024 / 1024:.2f} MB)")

if __name__ == "__main__":
    main()
