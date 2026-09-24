#!/usr/bin/env python3
"""convert_mbtiles_to_vnmb.py — Convert standard OpenStreetMap MBTiles (SQLite) into VNMB binary container.

Usage:
  python convert_mbtiles_to_vnmb.py vietnam.mbtiles --out data/speedmap/maptiles_osm.bin
  python convert_mbtiles_to_vnmb.py hanoi.mbtiles --min-zoom 9 --max-zoom 15

Features:
  - Direct extraction from SQLite MBTiles table
  - Automatic TMS (tile_row) to Slippy Map (y) coordinate conversion: y = (1 << z) - 1 - tile_row
  - Automatic PNG to JPEG conversion (75% quality) for ESP32 TJpgDec hardware decoder
  - Generates binary-searchable Spatial Index Table matching VietHUD's RasterMapManager
"""

import argparse
import io
import os
import sqlite3
import struct
import sys
import time
from pathlib import Path
from PIL import Image

MAGIC = b"VNMB"
VERSION = 1
HEADER_FMT = "<4sHBBIII44s"  # 64 bytes
assert struct.calcsize(HEADER_FMT) == 64

INDEX_ENTRY_FMT = "<QIHH"  # 16 bytes
assert struct.calcsize(INDEX_ENTRY_FMT) == 16


def make_tile_key(z, x, y):
    return (int(z) << 40) | (int(x) << 20) | int(y)


def convert_mbtiles(mbtiles_path, out_path, min_zoom=None, max_zoom=None, quality=75):
    mbtiles_path = Path(mbtiles_path)
    if not mbtiles_path.exists():
        print(f"Error: file not found: {mbtiles_path}")
        return False

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"Opening MBTiles: {mbtiles_path}")
    conn = sqlite3.connect(str(mbtiles_path))
    cursor = conn.cursor()

    # Inspect metadata
    metadata = {}
    try:
        for row in cursor.execute("SELECT name, value FROM metadata"):
            metadata[row[0]] = row[1]
    except Exception as e:
        print(f"Warning reading metadata: {e}")

    print(f"MBTiles Name: {metadata.get('name', 'N/A')}")
    print(f"Format: {metadata.get('format', 'unknown')}")

    # Query tile bounds
    cursor.execute("SELECT MIN(zoom_level), MAX(zoom_level), COUNT(*) FROM tiles")
    db_min_z, db_max_z, total_in_db = cursor.fetchone()
    print(f"Database zoom range: z{db_min_z}..z{db_max_z}, total tiles: {total_in_db:,}")

    actual_min_z = min_zoom if min_zoom is not None else db_min_z
    actual_max_z = max_zoom if max_zoom is not None else db_max_z

    query = "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles WHERE zoom_level >= ? AND zoom_level <= ?"
    cursor.execute(query, (actual_min_z, actual_max_z))

    temp_payload_file = out_path.with_suffix(".tmp_payload")
    payload_f = open(temp_payload_file, "wb")

    index_entries = []
    current_offset = 64  # Starts right after 64-byte header
    converted_count = 0
    start_time = time.time()

    print(f"Converting tiles for zoom levels z{actual_min_z}..z{actual_max_z}...")

    row_count = 0
    while True:
        rows = cursor.fetchmany(1000)
        if not rows:
            break

        for z, x, tile_row, raw_blob in rows:
            row_count += 1
            # Convert TMS tile_row to Slippy Map Y
            y = (1 << z) - 1 - tile_row
            key = make_tile_key(z, x, y)

            # Check image format (detect JPEG or PNG)
            is_jpeg = len(raw_blob) > 2 and raw_blob[0] == 0xFF and raw_blob[1] == 0xD8
            if is_jpeg:
                jpg_data = raw_blob
            else:
                try:
                    img = Image.open(io.BytesIO(raw_blob)).convert("RGB")
                    buf = io.BytesIO()
                    img.save(buf, format="JPEG", quality=quality, optimize=True)
                    jpg_data = buf.getvalue()
                except Exception as e:
                    continue

            data_len = len(jpg_data)
            if data_len > 65535:
                # Downsample if unusually large
                img = Image.open(io.BytesIO(jpg_data)).resize((256, 256))
                buf = io.BytesIO()
                img.save(buf, format="JPEG", quality=60)
                jpg_data = buf.getvalue()
                data_len = len(jpg_data)

            payload_f.write(jpg_data)
            index_entries.append((key, current_offset, data_len))
            current_offset += data_len
            converted_count += 1

            if converted_count % 5000 == 0:
                elapsed = time.time() - start_time
                print(f"Processed {converted_count:,} tiles ({converted_count/elapsed:.1f} tiles/s)...")

    payload_f.close()
    conn.close()

    print(f"Sorting {len(index_entries):,} tile index entries...")
    index_entries.sort(key=lambda item: item[0])

    index_offset = current_offset
    index_size = len(index_entries) * 16

    print(f"Writing final container: {out_path}...")
    with open(out_path, "wb") as out_f:
        # Write 64-byte Header
        hdr = struct.pack(
            HEADER_FMT,
            MAGIC,
            VERSION,
            int(actual_min_z),
            int(actual_max_z),
            len(index_entries),
            index_offset,
            index_size,
            b"\x00" * 44,
        )
        out_f.write(hdr)

        # Stream payload
        with open(temp_payload_file, "rb") as pf:
            while True:
                chunk = pf.read(1024 * 1024)
                if not chunk:
                    break
                out_f.write(chunk)

        # Write index entries
        for key, offset, size in index_entries:
            entry = struct.pack(INDEX_ENTRY_FMT, key, offset, size, 0)
            out_f.write(entry)

    if temp_payload_file.exists():
        temp_payload_file.unlink()

    total_mb = out_path.stat().st_size / (1024 * 1024)
    print(f"SUCCESS! Created {out_path} ({total_mb:.2f} MB, {len(index_entries):,} tiles).")
    return True


def main():
    parser = argparse.ArgumentParser(description="Convert MBTiles to VNMB for VietHUD")
    parser.add_argument("mbtiles", help="Path to input .mbtiles SQLite file")
    parser.add_argument("--out", default="data/speedmap/maptiles_osm.bin", help="Output .bin file path")
    parser.add_argument("--min-zoom", type=int, default=None, help="Minimum zoom to include")
    parser.add_argument("--max-zoom", type=int, default=None, help="Maximum zoom to include")
    parser.add_argument("--quality", type=int, default=75, help="JPEG quality (default 75)")
    args = parser.parse_args()

    convert_mbtiles(args.mbtiles, args.out, args.min_zoom, args.max_zoom, args.quality)


if __name__ == "__main__":
    main()
