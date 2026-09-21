#!/usr/bin/env python3
"""build_trafficsigns.py — Convert traffic_signs.csv into offline binary databases for ESP32.

Produces:
  <outdir>/speedmap/cameras.bin (CameraPoint[] — 100% compatible with SpeedMapFormat.h)
  <outdir>/speedmap/signs.bin   (TrafficSignPoint[] — full national traffic sign DB)
  <outdir>/speedmap/sounds/     (Vietnamese MP3 audio voice prompts)

Usage:
  python build_trafficsigns.py --csv path/to/traffic_signs.csv --out output_dir
"""
import argparse
import csv
import os
import shutil
import struct
import sys
import zlib
from pathlib import Path

# CameraPoint: struct { uint64 id; int32 latE7; int32 lonE7; int16 speedLimitKmh; uint16 directionDeg; }
CAMERA_FMT = "<QiihH"
assert struct.calcsize(CAMERA_FMT) == 20

# TrafficSignPoint: struct { uint32 id; int32 latE7; int32 lonE7; uint16 directionDeg; uint8 signType; uint8 speedLimitKmh; uint8 subType; uint8 flags; uint16 reserved; }
SIGN_FMT = "<IiiHBBBBH"
assert struct.calcsize(SIGN_FMT) == 20

SIGN_MAGIC = b"VNSG"
SIGN_VERSION = 1
SIGN_HEADER_FMT = "<4sHIHII" # magic(4), version(2), signCount(4), reserved(2), crc32(4), timestamp(4)
assert struct.calcsize(SIGN_HEADER_FMT) == 20

def to_e7(deg):
    return int(round(deg * 1e7))

def main():
    parser = argparse.ArgumentParser(description="Build offline traffic signs and camera binary database")
    parser.add_argument("--csv", default=r"C:\Users\phamq\Downloads\traffic_signs.csv", help="Path to traffic_signs.csv")
    parser.add_argument("--out", default=r"C:\Users\phamq\radar_car\tools\map_builder\output\speedmap", help="Output directory")
    parser.add_argument("--sound-dir", default=r"C:\Users\phamq\radar_car\tools\map_builder\wyn_assets\assets\flutter_assets\assets\sounds\vi", help="Directory containing Vietnamese audio MP3s")
    parser.add_argument("--copy-sounds", action="store_true", default=True, help="Copy MP3 voice prompts into output directory")
    args = parser.parse_args()

    csv_path = Path(args.csv)
    if not csv_path.exists():
        print(f"ERROR: CSV file not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Parsing {csv_path} ...")
    cameras = []
    signs = []

    cam_id_seq = 9000000001
    sign_id_seq = 1

    with open(csv_path, "r", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            lat = float(row["lat"])
            lon = float(row["lon"])
            speed = int(row.get("speed_limit", 0))
            raw_heading = float(row.get("heading", 0))
            # Heading in CSV is half-degrees (0..180). Multiply by 2 for 0..360 deg
            heading = int(round(raw_heading * 2.0)) % 360 if raw_heading > 0 else 0xFFFF
            sign_type = int(row.get("sign_type", 1))

            # Determine subtype: 0 = start/active, 1 = end
            sub_type = 1 if speed == 0 and sign_type in (1, 2, 3) else 0

            # 1. Type 4 is speed camera / enforcement camera -> goes into cameras.bin
            if sign_type == 4:
                cameras.append(dict(
                    id=cam_id_seq,
                    latE7=to_e7(lat),
                    lonE7=to_e7(lon),
                    speedLimitKmh=speed if speed > 0 else -1,
                    directionDeg=heading,
                ))
                cam_id_seq += 1

            # 2. All signs (including cameras and other sign types) go into signs.bin
            signs.append(dict(
                id=sign_id_seq,
                latE7=to_e7(lat),
                lonE7=to_e7(lon),
                directionDeg=heading,
                signType=sign_type,
                speedLimitKmh=speed if 0 <= speed <= 255 else 0,
                subType=sub_type,
                flags=0,
                reserved=0,
            ))
            sign_id_seq += 1

    print(f"Loaded {len(signs):,} signs in total, including {len(cameras):,} cameras.")

    # Write cameras.bin
    cameras_bin_path = out_dir / "cameras.bin"
    with open(cameras_bin_path, "wb") as f:
        for cam in cameras:
            f.write(struct.pack(
                CAMERA_FMT,
                cam["id"],
                cam["latE7"],
                cam["lonE7"],
                cam["speedLimitKmh"],
                cam["directionDeg"],
            ))
    print(f"-> Wrote {cameras_bin_path} ({cameras_bin_path.stat().st_size:,} bytes, {len(cameras):,} cameras)")

    # Write signs.bin
    signs_data = b""
    for s in signs:
        signs_data += struct.pack(
            SIGN_FMT,
            s["id"],
            s["latE7"],
            s["lonE7"],
            s["directionDeg"],
            s["signType"],
            s["speedLimitKmh"],
            s["subType"],
            s["flags"],
            s["reserved"],
        )

    crc = zlib.crc32(signs_data) & 0xFFFFFFFF
    header = struct.pack(SIGN_HEADER_FMT, SIGN_MAGIC, SIGN_VERSION, len(signs), 0, crc, 0)

    signs_bin_path = out_dir / "signs.bin"
    with open(signs_bin_path, "wb") as f:
        f.write(header)
        f.write(signs_data)
    print(f"-> Wrote {signs_bin_path} ({signs_bin_path.stat().st_size:,} bytes, {len(signs):,} traffic signs)")

    # Copy sounds if available
    sound_src = Path(args.sound_dir)
    if args.copy_sounds and sound_src.exists():
        sound_dst = out_dir / "sounds"
        sound_dst.mkdir(parents=True, exist_ok=True)
        count = 0
        for root, dirs, files in os.walk(sound_src):
            for file in files:
                if file.lower().endswith(".mp3"):
                    src_f = Path(root) / file
                    rel = src_f.relative_to(sound_src)
                    dst_f = sound_dst / rel
                    dst_f.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src_f, dst_f)
                    count += 1
        print(f"-> Copied {count} audio MP3 files to {sound_dst}")

    print("SUCCESS! All offline data packed.")

if __name__ == '__main__':
    main()
