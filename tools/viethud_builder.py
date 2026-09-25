#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
   VIETHUD ALL-IN-ONE MASTER DATA BUILDER & GITHUB OTA PUBLISHER (2026)
   Công cụ chuẩn hóa, tổng hợp, khử trùng lặp và tự động đồng bộ GitHub OTA
================================================================================
"""

import os
import sys
import csv
import struct
import math
import zlib
import time
import shutil
import hashlib
import datetime
import subprocess
import argparse
from collections import defaultdict, Counter

# Đảm bảo UTF-8 cho console Windows
try:
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
except Exception:
    pass

# -----------------------------------------------------------------------------
# CẤU TRÚC VÀ ĐỊNH NGHĨA CHUẨN VIETHUD FIRMWARE (SpeedMapFormat.h)
# -----------------------------------------------------------------------------
CAMERA_FMT = "<QiihH"
assert struct.calcsize(CAMERA_FMT) == 20

SIGN_FMT = "<IiiHBBBBH"
assert struct.calcsize(SIGN_FMT) == 20

SIGN_HEADER_FMT = "<4sHIHII"
assert struct.calcsize(SIGN_HEADER_FMT) == 20

SIGN_MAGIC = b"VNSG"
SIGN_VERSION = 1

# TrafficSignType Enum
SIGN_TYPE_UNKNOWN       = 0
SIGN_TYPE_SPEED_LIMIT   = 1 # P.127: Giới hạn tốc độ
SIGN_TYPE_RESIDENT_AREA = 2 # R.420 / R.421: Khu đông dân cư
SIGN_TYPE_NO_OVERTAKING = 3 # P.125 / DP.133: Cấm vượt
SIGN_TYPE_CAMERA        = 4 # Camera phạt nguội độc lập
SIGN_TYPE_TOLL_BOOTH    = 5 # P.135: Trạm thu phí BOT
SIGN_TYPE_TRAFFIC_LIGHT = 6 # Đèn tín hiệu / Camera vượt đèn đỏ
SIGN_TYPE_DANGER_OTHER  = 10 # Cao tốc, hầm, trạm dừng, cầu hẹp

SIGN_TYPE_NAMES = {
    SIGN_TYPE_SPEED_LIMIT: 'Camera bắn tốc độ / Giới hạn tốc độ (Type 1)',
    SIGN_TYPE_RESIDENT_AREA: 'Khu đông dân cư Vào/Ra (Type 2)',
    SIGN_TYPE_NO_OVERTAKING: 'Đoạn đường cấm vượt (Type 3)',
    SIGN_TYPE_CAMERA: 'Camera phạt nguội độc lập (Type 4)',
    SIGN_TYPE_TOLL_BOOTH: 'Trạm thu phí BOT (Type 5)',
    SIGN_TYPE_TRAFFIC_LIGHT: 'Đèn giao thông / Camera vượt đèn đỏ (Type 6)',
    SIGN_TYPE_DANGER_OTHER: 'Lối vào cao tốc / Hầm / Trạm dừng / Cầu hẹp (Type 10)'
}

# -----------------------------------------------------------------------------
# THÔNG SỐ GITHUB OTA MẶC ĐỊNH
# -----------------------------------------------------------------------------
DEFAULT_REPO_URL = "https://github.com/911273/VietHUD.git"
DEFAULT_BRANCH = "main"

CORE_VECTOR_FILES = [
    "cameras.bin",
    "signs.bin",
    "tiles.bin",
    "index.bin",
    "metadata.bin",
    "names.bin",
    "seg_names.bin"
]

# -----------------------------------------------------------------------------
# TOÁN HỌC KHÔNG GIAN
# -----------------------------------------------------------------------------
def haversine_m(lat1, lon1, lat2, lon2):
    R = 6371000
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlambda/2)**2
    return 2 * R * math.atan2(math.sqrt(a), math.sqrt(1 - a))

def heading_diff(h1, h2):
    diff = abs(h1 - h2) % 360
    return 360 - diff if diff > 180 else diff

def to_e7(val):
    return int(round(val * 1e7))

# -----------------------------------------------------------------------------
# PHÁT HIỆN Ổ THẺ NHỚ REMOVABLE
# -----------------------------------------------------------------------------
def find_removable_drives():
    drives = []
    try:
        cmd = 'powershell -NoProfile -Command "Get-Volume | Where-Object {$_.DriveType -eq \'Removable\' -and $_.DriveLetter -ne $null} | Select-Object -ExpandProperty DriveLetter"'
        res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        for line in res.stdout.strip().splitlines():
            line = line.strip()
            if line and len(line) == 1 and line.isalpha():
                drives.append(f"{line}:\\")
    except Exception:
        pass
    return drives

# -----------------------------------------------------------------------------
# TRÌNH ĐỌC DỮ LIỆU ĐA NGUỒN
# -----------------------------------------------------------------------------
def load_vietmap_csv(csv_path):
    points = []
    if not os.path.exists(csv_path):
        return points
    with open(csv_path, 'r', encoding='utf-8-sig', errors='ignore') as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return points
            
        header_lower = [h.strip().lower() for h in header]
        
        # Nhận diện cột chính xác theo tên
        lon_idx = lat_idx = type_idx = speed_idx = heading_idx = desc_idx = -1
        for idx, col in enumerate(header_lower):
            if col in ('kinh_do', 'lon', 'lng', 'longitude'):
                lon_idx = idx
            elif col in ('vi_do', 'lat', 'latitude'):
                lat_idx = idx
            elif col in ('ma_loai', 'type', 'm_type', 'stype'):
                type_idx = idx
            elif col in ('ten_loai_canh_bao', 'ten_loai', 'desc', 'description', 'name'):
                desc_idx = idx
            elif col in ('toc_do_kmh', 'toc_do', 'speed'):
                speed_idx = idx
            elif col in ('goc_huong_do', 'goc_huong', 'huong', 'heading', 'deg', 'angle'):
                heading_idx = idx
                
        # Dự phòng theo vị trí mặc định nếu tên cột không khớp
        if lon_idx == -1: lon_idx = 0
        if lat_idx == -1: lat_idx = 1
        if type_idx == -1: type_idx = 2
        if desc_idx == -1: desc_idx = 3 if len(header) > 3 else -1
        if speed_idx == -1: speed_idx = 4 if len(header) > 4 else -1
        if heading_idx == -1: heading_idx = 6 if len(header) > 6 else (5 if len(header) > 5 else -1)

        for r in reader:
            if not r or len(r) <= max(lon_idx, lat_idx):
                continue
            try:
                lon = float(r[lon_idx])
                lat = float(r[lat_idx])
                m_type = int(r[type_idx]) if type_idx >= 0 and len(r) > type_idx and r[type_idx] else 0
                desc = r[desc_idx] if desc_idx >= 0 and len(r) > desc_idx else ''
                speed = int(float(r[speed_idx])) if speed_idx >= 0 and len(r) > speed_idx and r[speed_idx] else 0
                heading = int(float(r[heading_idx])) if heading_idx >= 0 and len(r) > heading_idx and r[heading_idx] else 0
            except (ValueError, IndexError):
                continue
                
            if m_type == 1:
                u_type = SIGN_TYPE_SPEED_LIMIT
                is_cam = True
            elif m_type == 2:
                u_type = SIGN_TYPE_TRAFFIC_LIGHT
                is_cam = True
            elif m_type == 4:
                u_type = SIGN_TYPE_RESIDENT_AREA
                is_cam = False
            elif m_type == 10:
                u_type = SIGN_TYPE_NO_OVERTAKING
                is_cam = False
            elif m_type == 11:
                u_type = SIGN_TYPE_TOLL_BOOTH
                is_cam = False
            else:
                u_type = SIGN_TYPE_DANGER_OTHER
                is_cam = (speed > 0)

            points.append({
                'source': 'VietMap_M1',
                'lat': lat,
                'lon': lon,
                'type': u_type,
                'speed': speed,
                'heading': heading % 360,
                'is_camera': is_cam,
                'desc': desc if desc else f'Canh bao loai {m_type}'
            })
    return points

def load_papago_spc(spc_path):
    points = []
    if not os.path.exists(spc_path):
        return points
    file_size = os.path.getsize(spc_path)
    if file_size < 232780:
        return points

    with open(spc_path, 'rb') as f:
        f.seek(240)
        t1_records = [f.read(20) for _ in range(11627)]
        f.seek(232780)
        t2_records = [f.read(20) for _ in range(13476)]

    for rec in t1_records:
        if len(rec) < 20: continue
        lon_raw, lat_raw = struct.unpack('<II', rec[:8])
        h_raw = struct.unpack('<H', rec[14:16])[0]
        speed = rec[17]
        heading = (h_raw - 54272) % 360 if h_raw >= 54272 else h_raw
        points.append({
            'source': 'Papago_GoSafe',
            'lat': lat_raw / 1e6,
            'lon': lon_raw / 1e6,
            'type': SIGN_TYPE_SPEED_LIMIT,
            'speed': speed,
            'heading': heading % 360,
            'is_camera': True,
            'desc': f'Camera toc do {speed}km/h (Papago)'
        })

    papago_map = {
        2: ('Camera phat nguoi / Vuot den do', SIGN_TYPE_TRAFFIC_LIGHT, True),
        3: ('Camera giam sat giao thong', SIGN_TYPE_TRAFFIC_LIGHT, True),
        4: ('Doan duong cam vuot / Het cam vuot', SIGN_TYPE_NO_OVERTAKING, False),
        5: ('Tram thu phi BOT', SIGN_TYPE_TOLL_BOOTH, False),
        6: ('Den tin hieu giao thong / Nga tu', SIGN_TYPE_TRAFFIC_LIGHT, True),
        7: ('Duong ham / Cau vuot', SIGN_TYPE_DANGER_OTHER, False),
        8: ('Tram dung nghi', SIGN_TYPE_DANGER_OTHER, False),
        9: ('Cau hep / Doan nguy hiem', SIGN_TYPE_DANGER_OTHER, False)
    }

    for rec in t2_records:
        if len(rec) < 20: continue
        lon_raw, lat_raw = struct.unpack('<II', rec[:8])
        h_raw = struct.unpack('<H', rec[14:16])[0]
        speed = rec[17]
        p_type = rec[19]
        heading = (h_raw - 54272) % 360 if h_raw >= 54272 else h_raw
        label, stype, is_cam = papago_map.get(p_type, ('Bien bao giao thong', SIGN_TYPE_DANGER_OTHER, False))
        points.append({
            'source': 'Papago_GoSafe',
            'lat': lat_raw / 1e6,
            'lon': lon_raw / 1e6,
            'type': stype,
            'speed': speed,
            'heading': heading % 360,
            'is_camera': is_cam,
            'desc': f'{label} (Papago)'
        })
    return points

def load_signs_bin(bin_path, source_label='SDCard'):
    points = []
    if not os.path.exists(bin_path):
        return points
    with open(bin_path, 'rb') as f:
        hdr = f.read(20)
        if len(hdr) < 20 or hdr[:4] != SIGN_MAGIC:
            return points
        count = struct.unpack('<HIHII', hdr[4:])[1]
        for _ in range(count):
            data = f.read(20)
            if len(data) < 20: break
            sid, lat_e7, lon_e7, heading, stype, speed, subtype, flags, res = struct.unpack(SIGN_FMT, data)
            is_cam = (stype in (1, 4, 6) or speed > 0)
            
            type_desc_map = {
                1: f'Gioi han toc do {speed}km/h (SD)',
                2: 'Khu dong dan cu (SD)',
                3: 'Cam vuot / Het cam vuot (SD)',
                4: 'Camera phat nguoi (SD)',
                5: 'Tram thu phi (SD)',
                6: 'Den tin hieu / Nga tu (SD)',
                10: 'Canh bao khac (SD)'
            }
            
            points.append({
                'source': source_label,
                'lat': lat_e7 / 1e7,
                'lon': lon_e7 / 1e7,
                'type': stype,
                'speed': speed,
                'heading': heading % 360,
                'is_camera': is_cam,
                'desc': type_desc_map.get(stype, f'Bien bao loai {stype} ({source_label})')
            })
    return points

# -----------------------------------------------------------------------------
# THUẬT TOÁN BĂM LƯỚI KHÔNG GIAN ĐA TẦNG VÀ KHỬ TRÙNG LẶP
# -----------------------------------------------------------------------------
def synthesize_and_deduplicate(datasets):
    GRID_BASE = 0.0001 # ~11m
    unified = []
    grid = defaultdict(list)

    total_input = sum(len(ds) for ds in datasets)
    print(f"\n[XU LY] Bat dau tong hop {len(datasets)} nguon du lieu ({total_input:,} diem ban dau)...")

    # Pha 1: Nạp từng nguồn vào tập hợp và lọc trùng không gian
    inter_dups = 0
    for ds_idx, dataset in enumerate(datasets):
        added_in_ds = 0
        for p in dataset:
            gx = int(p['lat'] / GRID_BASE)
            gy = int(p['lon'] / GRID_BASE)
            is_dup = False

            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    for u_idx in grid.get((gx + dx, gy + dy), []):
                        u = unified[u_idx]
                        dist = haversine_m(p['lat'], p['lon'], u['lat'], u['lon'])
                        if dist <= 12.0:
                            same_type = (p['type'] == u['type'])
                            both_cam = (p['is_camera'] and u['is_camera'])
                            if same_type or both_cam:
                                hd = heading_diff(p['heading'], u['heading'])
                                if hd <= 45 or p['heading'] in (0, 0xFFFF) or u['heading'] in (0, 0xFFFF):
                                    is_dup = True
                                    if u['speed'] == 0 and p['speed'] > 0:
                                        u['speed'] = p['speed']
                                    if both_cam and u['type'] != p['type']:
                                        if p['type'] == SIGN_TYPE_SPEED_LIMIT and p['speed'] > 0:
                                            u['type'] = SIGN_TYPE_SPEED_LIMIT
                                            u['speed'] = p['speed']
                                    break
                    if is_dup: break
                if is_dup: break

            if is_dup:
                inter_dups += 1
            else:
                added_in_ds += 1
                unified.append(p)
                grid[(gx, gy)].append(len(unified) - 1)
                
        ds_name = dataset[0]['source'] if dataset else f'Nguon {ds_idx+1}'
        print(f"  + Nguon '{ds_name}': Bo sung {added_in_ds:,} diem doc quyen")

    print(f"  -> Tong so diem so bo sau hop nhat: {len(unified):,} diem (Loai bo {inter_dups:,} diem trung)")

    # Pha 2: Quét lưới siêu nhỏ (1m - 5m)
    print("  -> Ra soat luoi sieu nho (1m - 5m) de lam sach triet de...")
    GRID_1M = 0.000009 # ~1.0m
    grid_1m = defaultdict(list)
    for idx, p in enumerate(unified):
        gx = int(p['lat'] / GRID_1M)
        gy = int(p['lon'] / GRID_1M)
        grid_1m[(gx, gy)].append(idx)

    to_drop = set()
    exact_zero_drop = 0
    micro_drop = 0
    opp_kept = 0
    cross_kept = 0

    for idx, p in enumerate(unified):
        if idx in to_drop: continue
        gx = int(p['lat'] / GRID_1M)
        gy = int(p['lon'] / GRID_1M)

        for dx in range(-5, 6): # ~5m
            for dy in range(-5, 6):
                for o_idx in grid_1m.get((gx + dx, gy + dy), []):
                    if o_idx <= idx or o_idx in to_drop: continue
                    o = unified[o_idx]
                    dist = haversine_m(p['lat'], p['lon'], o['lat'], o['lon'])
                    if dist <= 5.0:
                        same_type = (p['type'] == o['type'])
                        both_cam = (p['is_camera'] and o['is_camera'])
                        if same_type or both_cam:
                            hd = heading_diff(p['heading'], o['heading'])
                            if hd <= 45 or p['heading'] in (0, 0xFFFF) or o['heading'] in (0, 0xFFFF):
                                to_drop.add(o_idx)
                                if dist < 0.1:
                                    exact_zero_drop += 1
                                else:
                                    micro_drop += 1
                                if p['speed'] == 0 and o['speed'] > 0:
                                    p['speed'] = o['speed']
                            elif hd >= 135:
                                opp_kept += 1
                            else:
                                cross_kept += 1

    final_clean = [p for idx, p in enumerate(unified) if idx not in to_drop]
    print(f"  + Khu trung lap tuyet doi (<0.1m): {exact_zero_drop:,} diem")
    print(f"  + Khu trung lap vi mo (1m - 5m):    {micro_drop:,} diem")
    print(f"  + Bao toan canh bao doi xung 2 chieu: {opp_kept:,} cap")
    print(f"  + Bao toan canh bao nga ba/nga tu:   {cross_kept:,} cap")
    print(f"  ==> TONG CONG SAU LAM SACH: {len(final_clean):,} DIEM")
    return final_clean

# -----------------------------------------------------------------------------
# ĐÓNG GÓI NHỊ PHÂN VÀ XUẤT RA THẺ NHỚ
# -----------------------------------------------------------------------------
def export_viethud_package(clean_points, ref_map_dir, target_dirs, mode='vector'):
    camera_records = []
    sign_records = []
    cam_id_counter = 8000000000
    sign_id_counter = 1

    for p in clean_points:
        lat_e7 = to_e7(p['lat'])
        lon_e7 = to_e7(p['lon'])
        heading_deg = p['heading'] % 360
        speed = p['speed']

        if p['is_camera']:
            camera_records.append(struct.pack(
                CAMERA_FMT,
                cam_id_counter,
                lat_e7,
                lon_e7,
                heading_deg,
                speed
            ))
            cam_id_counter += 1

        sign_records.append(struct.pack(
            SIGN_FMT,
            sign_id_counter,
            lat_e7,
            lon_e7,
            heading_deg,
            p['type'],
            speed,
            0,
            0,
            0
        ))
        sign_id_counter += 1

    cameras_data = b"".join(camera_records)

    timestamp = int(time.time())
    total_signs = len(sign_records)
    signs_payload = b"".join(sign_records)
    crc = zlib.crc32(signs_payload) & 0xFFFFFFFF
    header = struct.pack(SIGN_HEADER_FMT, SIGN_MAGIC, SIGN_VERSION, total_signs, 0, timestamp, crc)
    signs_data = header + signs_payload

    print("\n[DONG GOI] Dang xuat file nhi phan chuan VietHUD...")
    print(f"  + cameras.bin : {len(cameras_data):,} bytes ({len(camera_records):,} camera records)")
    print(f"  + signs.bin   : {len(signs_data):,} bytes ({total_signs:,} signs records)")

    if mode == 'vector':
        map_files = ["metadata.bin", "index.bin", "tiles.bin", "names.bin", "seg_names.bin"]
        mode_desc = "BAN DO VECTOR + TEN DUONG (Toi gian ~16.6MB, khong kem anh raster)"
    elif mode == 'alerts':
        map_files = []
        mode_desc = "CHI DU LIEU CANH BAO GIAO THONG (Alerts only ~3MB)"
    else:
        map_files = ["metadata.bin", "index.bin", "tiles.bin", "names.bin", "seg_names.bin", "maptiles.bin"]
        mode_desc = "BAN DO DAY DU (Full ~460MB, bao gom ca anh raster maptiles.bin)"

    print(f"  * Che do dong goi: {mode_desc}")

    for desc, dest_dir in target_dirs:
        try:
            os.makedirs(dest_dir, exist_ok=True)
            v1_tiles = os.path.join(dest_dir, 'tiles')
            if os.path.exists(v1_tiles):
                try: shutil.rmtree(v1_tiles)
                except Exception: pass

            # Tự động sao lưu .bak nếu là thẻ nhớ vật lý
            cam_path = os.path.join(dest_dir, 'cameras.bin')
            sign_path = os.path.join(dest_dir, 'signs.bin')
            if 'MicroSD' in desc:
                if os.path.exists(cam_path) and not os.path.exists(cam_path + '.bak'):
                    shutil.copy2(cam_path, cam_path + '.bak')
                if os.path.exists(sign_path) and not os.path.exists(sign_path + '.bak'):
                    shutil.copy2(sign_path, sign_path + '.bak')

            # Ghi cameras.bin & signs.bin
            with open(cam_path, 'wb') as f:
                f.write(cameras_data)
            with open(sign_path, 'wb') as f:
                f.write(signs_data)

            # Ghi file tra cứu CSV
            csv_path = os.path.join(dest_dir, 'combined_traffic_alerts.csv')
            with open(csv_path, 'w', encoding='utf-8-sig', newline='') as f:
                w = csv.writer(f)
                w.writerow(['id', 'kinh_do', 'vi_do', 'loai_canh_bao', 'ma_loai', 'toc_do_kmh', 'huong_do', 'la_camera', 'nguon_du_lieu'])
                for idx, p in enumerate(clean_points, start=1):
                    w.writerow([
                        idx, f"{p['lon']:.6f}", f"{p['lat']:.6f}", p['desc'],
                        p['type'], p['speed'], p['heading'], 1 if p['is_camera'] else 0, p['source']
                    ])

            # Nếu ở chế độ vector, dọn file ảnh maptiles.bin thừa nếu có trong thư mục VietHUD_SDCard_Ready
            if mode in ('vector', 'alerts') and 'VietHUD_SDCard_Ready' in dest_dir:
                old_raster = os.path.join(dest_dir, 'maptiles.bin')
                if os.path.exists(old_raster):
                    try: os.remove(old_raster)
                    except Exception: pass

            # Đồng bộ các file bản đồ vector và âm thanh từ ref_map_dir
            if ref_map_dir and os.path.exists(ref_map_dir):
                for mf in map_files:
                    src_f = os.path.join(ref_map_dir, mf)
                    dst_f = os.path.join(dest_dir, mf)
                    if os.path.exists(src_f) and os.path.abspath(src_f) != os.path.abspath(dst_f):
                        if not os.path.exists(dst_f) or os.path.getsize(src_f) != os.path.getsize(dst_f):
                            shutil.copyfile(src_f, dst_f)
                
                sounds_src = os.path.join(ref_map_dir, 'sounds')
                sounds_dst = os.path.join(dest_dir, 'sounds')
                if os.path.exists(sounds_src) and os.path.abspath(sounds_src) != os.path.abspath(sounds_dst):
                    if not os.path.exists(sounds_dst):
                        shutil.copytree(sounds_src, sounds_dst)

            upgrade_speedmap_names_v2(dest_dir)
            print(f"  [OK] Dong bo thanh cong vao: {desc} -> {dest_dir}")
        except Exception as e:
            print(f"  [CANH BAO] Khong the ghi vao {desc}: {e}")

# -----------------------------------------------------------------------------
# QUY CHUẨN ĐỊNH DẠNG TÊN ĐƯỜNG NAMES.BIN & SEG_NAMES.BIN V2 (2026)
# -----------------------------------------------------------------------------
VNNM_MAGIC = b"VNNM"

def upgrade_speedmap_names_v2(speedmap_dir):
    """
    Tự động kiểm tra và nâng cấp names.bin & seg_names.bin lên chuẩn V2:
      - names.bin    : magic[4]="VNNM", version(u16)=2, count(u32), poolBytes(u32), reserved(u16)=0
      - seg_names.bin: mảng uint32 (<I, 4 bytes mỗi segment), index = segId, giá trị = nameId
    """
    names_path = os.path.join(speedmap_dir, "names.bin")
    seg_names_path = os.path.join(speedmap_dir, "seg_names.bin")
    
    # 1. Nâng cấp names.bin
    if os.path.exists(names_path):
        try:
            with open(names_path, "rb") as f:
                data = f.read()
            if len(data) >= 16 and data[:4] == VNNM_MAGIC:
                version = struct.unpack("<H", data[4:6])[0]
                if version == 1:
                    count_v1, pool_bytes, res32 = struct.unpack("<HII", data[6:16])
                    v2_header = struct.pack("<4sHIIH", VNNM_MAGIC, 2, count_v1, pool_bytes, 0)
                    with open(names_path, "wb") as f:
                        f.write(v2_header + data[16:])
                    print(f"  [V2 UPGRADE] Da nang cap names.bin sang V2 (u32 count={count_v1:,}): {names_path}")
        except Exception as e:
            print(f"  [CANH BAO] Khong the kiem tra names.bin: {e}")

    # 2. Nâng cấp seg_names.bin
    if os.path.exists(seg_names_path):
        try:
            with open(seg_names_path, "rb") as f:
                data = f.read()
            file_size = len(data)
            # Nếu file_size chia hết cho 2 nhưng không phải định dạng V2 (u32)
            # Kiểm tra nếu kích thước tương ứng với file u16
            if file_size > 0:
                is_u32 = False
                if file_size % 4 == 0 and file_size >= 16:
                    check_cnt = min(file_size // 4, 100)
                    entries_u16 = struct.unpack(f"<{check_cnt * 2}H", data[:check_cnt * 4])
                    odd_zeros = sum(1 for i in range(1, len(entries_u16), 2) if entries_u16[i] == 0)
                    even_nonzeros = sum(1 for i in range(0, len(entries_u16), 2) if entries_u16[i] != 0)
                    if odd_zeros >= (len(entries_u16) // 2) - 2 and (even_nonzeros > 0 or check_cnt < 5):
                        is_u32 = True
                if not is_u32:
                    num_segs = file_size // 2
                    u16_entries = struct.unpack(f"<{num_segs}H", data[:num_segs * 2])
                    v2_data = struct.pack(f"<{num_segs}I", *u16_entries)
                    bak_path = seg_names_path + ".v1.bak"
                    if not os.path.exists(bak_path):
                        with open(bak_path, "wb") as f:
                            f.write(data)
                    with open(seg_names_path, "wb") as f:
                        f.write(v2_data)
                    print(f"  [V2 UPGRADE] Da nang cap seg_names.bin sang V2 (u32 per segment, {num_segs:,} segs): {seg_names_path}")
        except Exception as e:
            print(f"  [CANH BAO] Khong the kiem tra seg_names.bin: {e}")

# -----------------------------------------------------------------------------
# CÔNG CỤ TÍNH SHA-256 VÀ ĐẨY LÊN GITHUB OTA
# -----------------------------------------------------------------------------
def calculate_sha256(filepath):
    """Tính mã băm SHA-256 chuẩn của một file."""
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()

def run_git_cmd(cmd_list, cwd):
    """Thực thi lệnh git với kiểm tra lỗi."""
    cmd_str = " ".join(cmd_list)
    print(f"  [GIT] {cmd_str}")
    res = subprocess.run(cmd_list, cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if res.returncode != 0:
        err_msg = res.stderr.strip() or res.stdout.strip()
        print(f"  [GIT ERROR] {err_msg}")
        return False, err_msg
    return True, res.stdout.strip()

def build_readme_content(version, files_info, update_url):
    """Tạo nội dung README.md trực quan cho GitHub Repository."""
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = [
        "# 🚗 VietHUD - Dữ Liệu Giao Thông & Bản Đồ Ngoại Tuyến (Offline Traffic Data)",
        "",
        "> Kho lưu trữ chính thức đồng bộ dữ liệu giao thông, camera phạt nguội và biển báo cho thiết bị **VietHUD**.",
        "",
        f"**Phiên bản phát hành (Version):** `{version}`  ",
        f"**Thời gian cập nhật:** `{now_str}`  ",
        f"**Đường dẫn OTA tải về (VietHUD OTA URL):**  ",
        f"```text\n{update_url}\n```",
        "",
        "---",
        "",
        "## 📦 Danh Sách Dữ Liệu Đồng Bộ (Core SpeedMap Files)",
        "",
        "| File | Kích thước | Mã băm SHA-256 | Mô tả |",
        "| :--- | :--- | :--- | :--- |"
    ]
    
    file_descs = {
        "cameras.bin": "Dữ liệu vị trí camera phạt nguội & camera bắn tốc độ toàn quốc",
        "signs.bin": "Dữ liệu biển báo giao thông (khu dân cư, cấm vượt, trạm thu phí, ...)",
        "tiles.bin": "Mạng lưới đường bộ vector & giới hạn tốc độ chi tiết",
        "index.bin": "Lưới chỉ mục không gian (Spatial Index) tra cứu nhanh",
        "metadata.bin": "Thông số khung tọa độ & cấu hình bản đồ",
        "names.bin": "Từ điển tên đường phố toàn quốc",
        "seg_names.bin": "Ánh xạ định danh phân đoạn đường sang tên phố"
    }

    for item in files_info:
        fname = item["name"]
        sz_kb = item["size"] / 1024
        sz_str = f"{sz_kb:,.1f} KB" if sz_kb < 1024 else f"{sz_kb/1024:,.2f} MB"
        desc = file_descs.get(fname, "Dữ liệu bổ trợ")
        lines.append(f"| `{fname}` | {sz_str} | `{item['sha'][:16]}...` | {desc} |")

    lines.extend([
        "",
        "---",
        "",
        "## 🛠 Hướng Dẫn Cấu Hình Thiết Bị VietHUD Cập Nhật OTA",
        "1. Trên thiết bị VietHUD, vào mục **Cài đặt (Settings) -> WiFi** (hoặc truy cập WebPortal tại `http://192.168.4.1` khi kết nối WiFi phát từ VietHUD).",
        f"2. Tại ô **Data Update URL**, điền chính xác đường dẫn sau:",
        f"   ```text\n   {update_url}\n   ```",
        "3. Kết nối VietHUD vào Hotspot Wi-Fi của điện thoại.",
        "4. Bấm **Kiểm tra cập nhật (Check Update)** trên WebPortal hoặc màn hình HUD. Thiết bị sẽ tự động tải các file thay đổi, ghi vào thẻ nhớ MicroSD và khởi động lại.",
        ""
    ])
    return "\n".join(lines)

def publish_to_github(speedmap_src, repo_url=DEFAULT_REPO_URL, branch=DEFAULT_BRANCH, dry_run=False):
    """
    Tự động tính mã băm SHA-256, sinh manifest.txt và đẩy lên GitHub.
    """
    print("\n" + "="*80)
    print("        TIEN TRINH TU DONG DAY DU LIEU LEN GITHUB OTA")
    print("="*80)
    print(f"  * Thu muc speedmap nguon : {speedmap_src}")
    print(f"  * GitHub Repository     : {repo_url}")
    print(f"  * Nhanh (Branch)        : {branch}")
    print("-" * 80)

    if not speedmap_src or not os.path.exists(speedmap_src):
        print(f"[LOI] Thu muc speedmap '{speedmap_src}' khong ton tai!")
        return False

    # 0. Nâng cấp names.bin & seg_names.bin lên chuẩn V2 nếu cần
    upgrade_speedmap_names_v2(speedmap_src)

    # 1. Quét và tính SHA-256
    now = datetime.datetime.now()
    version_str = now.strftime("%Y.%m.%d.%H%M")
    files_info = []

    print("\n[1/4] Dang tinh toan ma bam SHA-256 cac file vector...")
    for fname in CORE_VECTOR_FILES:
        fpath = os.path.join(speedmap_src, fname)
        if not os.path.exists(fpath):
            print(f"  [CANH BAO] File {fname} khong ton tai!")
            continue
        sz = os.path.getsize(fpath)
        sha = calculate_sha256(fpath)
        files_info.append({
            "name": fname,
            "size": sz,
            "sha": sha
        })
        sz_kb = sz / 1024
        sz_str = f"{sz_kb:,.1f} KB" if sz_kb < 1024 else f"{sz_kb/1024:,.2f} MB"
        print(f"  + {fname:<15} : {sz_str:>10} | SHA: {sha[:16]}...")

    if not files_info:
        print("[LOI] Khong tim thay file vector hop le de dong goi manifest!")
        return False

    # 2. Sinh manifest.txt
    manifest_lines = [f"version {version_str}"]
    for item in files_info:
        manifest_lines.append(f"{item['name']} {item['size']} {item['sha']}")
    manifest_content = "\n".join(manifest_lines) + "\n"

    src_manifest = os.path.join(speedmap_src, "manifest.txt")
    with open(src_manifest, "w", encoding="utf-8") as f:
        f.write(manifest_content)
    print(f"\n[2/4] Da sinh file manifest.txt: {src_manifest}")

    # 3. Chuẩn bị Git Staging
    script_dir = os.path.dirname(os.path.abspath(__file__))
    git_stage_dir = os.path.join(script_dir, "VietHUD_Git_Staging")
    os.makedirs(git_stage_dir, exist_ok=True)
    git_speedmap_dir = os.path.join(git_stage_dir, "speedmap")
    os.makedirs(git_speedmap_dir, exist_ok=True)

    print(f"\n[3/4] Dang sao chep du lieu vao thu muc Git Staging...")
    for item in files_info:
        shutil.copy2(os.path.join(speedmap_src, item["name"]), os.path.join(git_speedmap_dir, item["name"]))
    shutil.copy2(src_manifest, os.path.join(git_speedmap_dir, "manifest.txt"))

    # Sao chép thư viện âm thanh nếu có
    sounds_src = os.path.join(speedmap_src, "sounds")
    sounds_dst = os.path.join(git_speedmap_dir, "sounds")
    if os.path.exists(sounds_src):
        if os.path.exists(sounds_dst):
            shutil.rmtree(sounds_dst)
        shutil.copytree(sounds_src, sounds_dst)
        print("  + Da sao chep thu vien am thanh (sounds/)")

    update_url = f"https://raw.githubusercontent.com/911273/VietHUD/{branch}/speedmap/"
    readme_content = build_readme_content(version_str, files_info, update_url)
    with open(os.path.join(git_stage_dir, "README.md"), "w", encoding="utf-8") as f:
        f.write(readme_content)

    if dry_run:
        print("\n[CHẾ ĐỘ DRY-RUN] Đã kiểm tra thành công, bỏ qua bước push Git.")
        return True

    # 4. Thực thi Git Push
    print(f"\n[4/4] Dang day (Push) du lieu len GitHub: {repo_url}...")
    git_dir = os.path.join(git_stage_dir, ".git")
    if not os.path.exists(git_dir):
        run_git_cmd(["git", "init"], git_stage_dir)
        run_git_cmd(["git", "remote", "add", "origin", repo_url], git_stage_dir)
        run_git_cmd(["git", "branch", "-M", branch], git_stage_dir)
    else:
        run_git_cmd(["git", "remote", "set-url", "origin", repo_url], git_stage_dir)

    run_git_cmd(["git", "add", "."], git_stage_dir)
    status_ok, status_out = run_git_cmd(["git", "status", "--porcelain"], git_stage_dir)
    if not status_out.strip():
        print("  [THONG BAO] Du lieu tren GitHub da o trang thai moi nhat, khong co thay doi!")
    else:
        commit_msg = f"VietHUD Data Update v{version_str}"
        run_git_cmd(["git", "commit", "-m", commit_msg], git_stage_dir)
        push_ok, push_err = run_git_cmd(["git", "push", "-u", "origin", branch], git_stage_dir)
        if not push_ok:
            print("  [THU LAI] Dang thu fetch va push lai...")
            run_git_cmd(["git", "pull", "--rebase", "origin", branch], git_stage_dir)
            push_ok, push_err = run_git_cmd(["git", "push", "-u", "origin", branch], git_stage_dir)
            if not push_ok:
                print(f"\n[LOI PUSH GIT] Khong the day len GitHub: {push_err}")
                print("Vui long xac nhan dang nhap Git Credential Manager hoac kiem tra mang.")
                return False

    print("\n" + "=" * 80)
    print(" 🎉 CHUC MUNG! DU LIEU VIETHUD DA DUOC DAY LEN GITHUB THANH CONG!")
    print("=" * 80)
    print(f"  * Phien ban moi       : {version_str}")
    print(f"  * GitHub Web View     : https://github.com/911273/VietHUD")
    print(f"  * URL cau hinh OTA cho VietHUD:")
    print(f"    --> {update_url}")
    print("=" * 80)
    print("\n[HUONG DAN CAP NHAT TREN THIET BI VIETHUD]:")
    print(f"1. Mo trinh duyet tren dien thoai vao WebPortal VietHUD.")
    print(f"2. Tai muc 'Data Update URL', dan duong dan tren:")
    print(f"   {update_url}")
    print(f"3. Nhan 'Luu' va 'Kiem tra cap nhat'. Thiet bi se tu dong tai toan bo du lieu!")
    print("=" * 80)
    return True

# -----------------------------------------------------------------------------
# HÀM CHÍNH (MAIN)
# -----------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="VietHUD All-in-One Master Data Builder & GitHub OTA Tool")
    parser.add_argument('--input', '-i', default=None, help="Thu muc goc chua du lieu moi")
    parser.add_argument('--sd-drive', '-s', default=None, help="Ky tu o dia the nho (vi du: E hoac E:\\)")
    parser.add_argument('--ref-map', '-r', default=None, help="Thu muc chua ban do goc va am thanh")
    parser.add_argument('--mode', '-m', choices=['vector', 'full', 'alerts'], default=None,
                        help="Che do dong goi: vector (mac dinh ~16MB), full (~460MB), alerts (~3MB)")
    parser.add_argument('--no-raster', '--vector-only', action='store_true', dest='no_raster',
                        help="Chi xuat ban do vector va canh bao, khong kem anh raster (maptiles.bin)")
    parser.add_argument('--full', action='store_true',
                        help="Xuat day du bao gom ca anh raster (maptiles.bin)")
    parser.add_argument('--github', '-g', action='store_true',
                        help="Tu dong day du lieu len GitHub sau khi dong goi")
    parser.add_argument('--github-only', action='store_true',
                        help="Chi day du lieu san co trong VietHUD_SDCard_Ready len GitHub (khong convert lai)")
    parser.add_argument('--repo', default=DEFAULT_REPO_URL, help=f"URL GitHub Repository (Mac dinh: {DEFAULT_REPO_URL})")
    parser.add_argument('--branch', default=DEFAULT_BRANCH, help=f"Nhanh git (Mac dinh: {DEFAULT_BRANCH})")
    parser.add_argument('--dry-run', action='store_true', help="Kiem tra thu khong thuc su push git")
    
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Xác định thư mục input
    input_dir = args.input
    if not input_dir:
        candidates = [
            r'C:\Users\phamq\Downloads\Data',
            script_dir,
            os.path.join(script_dir, 'Data')
        ]
        for c in candidates:
            if os.path.exists(c):
                input_dir = c
                break
    if not input_dir or not os.path.exists(input_dir):
        input_dir = script_dir

    ready_pkg = os.path.join(input_dir, "VietHUD_SDCard_Ready", "speedmap")

    # XỬ LÝ TRƯỜNG HỢP: --github-only
    if args.github_only:
        publish_to_github(ready_pkg, repo_url=args.repo, branch=args.branch, dry_run=args.dry_run)
        return

    # NẾU KHÔNG CÓ THAM SỐ GÌ TRUYỀN VÀO -> HIỆN MENU TƯƠNG TÁC
    auto_push_git = args.github
    if len(sys.argv) == 1:
        print("=" * 80)
        print("       VIETHUD ALL-IN-ONE MASTER TOOL: CONVERT DATA & GITHUB OTA")
        print("=" * 80)
        print("  Vui long chon thao tac ban muon thuc hien:")
        print()
        print("   [1] DONG GOI BAN DO VECTOR (~16.6MB, Khuyen dung - Chep the nho)")
        print("       + Canh bao camera, bien bao, tuyen duong vector & am thanh")
        print("       - Khong kem file anh raster nang 448MB")
        print()
        print("   [2] DONG GOI BAN DO DAY DU (FULL - ~460MB)")
        print("       + Bao gom toan bo muc [1] va them anh raster maptiles.bin")
        print()
        print("   [3] ALL-IN-ONE: CONVERT DU LIEU + TU DONG DAY LEN GITHUB OTA (Khuyen dung)")
        print("       + Tu dong doc CSV/OSM, dong goi vector va push thang len GitHub")
        print("       + Cung cap URL de thiet bi VietHUD tu dong tai ve qua Wi-Fi")
        print()
        print("   [4] CHI DAY DU LIEU SAN CO LEN GITHUB OTA (Khong convert lai)")
        print("       + Tinh SHA-256 tren 'VietHUD_SDCard_Ready/speedmap' va day len GitHub")
        print()
        print("   [5] CHI DU LIEU CANH BAO GIAO THONG (Alerts Only - ~3MB)")
        print("   [0] Thoat")
        print("=" * 80)

        choice = input("Nhap lua chon cua ban [1/2/3/4/5/0] (Nhan Enter mac dinh la 3): ").strip()
        if choice == '0':
            print("Tam biet!")
            return
        elif choice == '1':
            mode = 'vector'
            auto_push_git = False
        elif choice == '2':
            mode = 'full'
            auto_push_git = False
        elif choice == '4':
            publish_to_github(ready_pkg, repo_url=args.repo, branch=args.branch, dry_run=args.dry_run)
            return
        elif choice == '5':
            mode = 'alerts'
            auto_push_git = False
        else: # Mặc định là 3 (All-in-one)
            mode = 'vector'
            auto_push_git = True
    else:
        # Xác định chế độ xuất từ tham số CLI
        if args.full:
            mode = 'full'
        elif args.no_raster:
            mode = 'vector'
        elif args.mode:
            mode = args.mode
        else:
            mode = 'vector'

    print("\n" + "="*80)
    print("      VIETHUD MASTER DATA BUILDER & SD-CARD SYNC TOOL (2026)")
    print(f"      Che do: {'BAN DO VECTOR + TEN DUONG (Khong anh raster)' if mode == 'vector' else ('BAN DO DAY DU (FULL)' if mode == 'full' else 'CHI CANH BAO GIAO THONG')}")
    print("="*80)

    # Xác định thư mục bản đồ gốc
    ref_map_dir = args.ref_map
    if not ref_map_dir:
        map_candidates = [
            r'C:\Users\phamq\radar_car\data\speedmap',
            os.path.join(input_dir, 'speedmap'),
            os.path.join(input_dir, 'VietHUD_SDCard_Ready', 'speedmap')
        ]
        for mc in map_candidates:
            if os.path.exists(mc) and os.path.exists(os.path.join(mc, 'metadata.bin')):
                ref_map_dir = mc
                break
    if not ref_map_dir:
        ref_map_dir = r'C:\Users\phamq\radar_car\data\speedmap'

    print(f"[*] Thu muc du lieu nguon: {input_dir}")
    print(f"[*] Thu muc ban do he thong: {ref_map_dir}")

    # 1. Tìm các nguồn dữ liệu cảnh báo giao thông
    datasets = []

    # Nguồn 1: Tìm tất cả file CSV VietMap (bỏ qua các thư mục output)
    ignored_dir_names = {'output_viethud', 'vietmap_output', 'viethud_output', 'viethud_sdcard_ready', 'speedmap', 'viethud_git_staging'}
    csv_candidates = []
    for root, dirs, files in os.walk(input_dir):
        dirs[:] = [d for d in dirs if d.lower() not in ignored_dir_names]
        for f in files:
            f_lower = f.lower()
            if f_lower.endswith('.csv') and 'combined' not in f_lower:
                csv_candidates.append(os.path.join(root, f))

    if csv_candidates:
        csv_candidates.sort(key=lambda x: os.path.getsize(x), reverse=True)
        for csv_f in csv_candidates:
            pts = load_vietmap_csv(csv_f)
            if pts:
                datasets.append(pts)
                print(f"  [1] Tim thay CSV VietMap: {os.path.basename(csv_f)} ({len(pts):,} diem)")

    # Nguồn 2: Dữ liệu GoSafe Papago SPC
    spc_candidates = []
    for root, dirs, files in os.walk(input_dir):
        dirs[:] = [d for d in dirs if d.lower() not in ignored_dir_names]
        for f in files:
            if f.lower().endswith('.spc') or 'spc' in f.lower():
                spc_candidates.append(os.path.join(root, f))
    if spc_candidates:
        spc_candidates.sort(key=lambda x: os.path.getmtime(x), reverse=True)
        latest_spc = spc_candidates[0]
        pp_pts = load_papago_spc(latest_spc)
        if pp_pts:
            datasets.append(pp_pts)
            print(f"  [2] Tim thay Papago GoSafe SPC: {latest_spc} ({len(pp_pts):,} diem)")

    # Nguồn 3: Dữ liệu thẻ nhớ / backup WYN gốc
    sd_bak_paths = [
        os.path.join(input_dir, 'WYN_SpeedMap_Ready', 'speedmap', 'signs.bin'),
        r'C:\Users\phamq\radar_car\tools\map_builder\output\speedmap\signs.bin',
        r'E:\speedmap\signs.bin.bak',
        r'E:\speedmap\signs.bin',
        os.path.join(input_dir, 'output_viethud', 'speedmap', 'signs.bin'),
        os.path.join(ref_map_dir, 'signs.bin.bak'),
        os.path.join(ref_map_dir, 'signs.bin'),
    ]
    for p in sd_bak_paths:
        if os.path.exists(p):
            sd_pts = load_signs_bin(p, source_label='SDCard_WYN')
            if sd_pts:
                datasets.append(sd_pts)
                print(f"  [3] Tim thay du lieu the nho WYN: {p} ({len(sd_pts):,} diem)")
                break

    if not datasets:
        print("[LOI] Khong tim thay bat ky nguon du lieu nao de tong hop!")
        return

    # 2. Hợp nhất và khử trùng lặp
    clean_points = synthesize_and_deduplicate(datasets)

    # 3. Xác định các mục tiêu xuất dữ liệu
    target_dirs = []
    target_dirs.append(("Goi hoan chinh copy ngay (VietHUD_SDCard_Ready)", ready_pkg))

    if os.path.exists(ref_map_dir):
        target_dirs.append(("Thu muc du lieu firmware VietHUD", ref_map_dir))

    removable = [args.sd_drive] if args.sd_drive else find_removable_drives()
    if removable:
        for drive in removable:
            sd_speedmap = os.path.join(drive.rstrip('\\') + '\\', 'speedmap')
            target_dirs.append((f"The nho MicroSD ({drive})", sd_speedmap))
    else:
        print("\n  * Thong bao: Hien tai chua cam truc tiep the nho MicroSD.")
        print(f"    Goi du lieu hoan chinh se duoc luu san tai thu muc:\n    -> {ready_pkg}")

    # 4. Đóng gói và ghi ra các đích
    export_viethud_package(clean_points, ref_map_dir, target_dirs, mode=mode)

    # 5. Thống kê
    types = Counter(p['type'] for p in clean_points)
    total_cams = sum(1 for p in clean_points if p['is_camera'])

    print("\n" + "="*80)
    print("THONG KE CHI TIET DIEM CANH BAO SAU CUNG TOAN QUOC:")
    print("="*80)
    for t, cnt in sorted(types.items()):
        print(f"  * {SIGN_TYPE_NAMES.get(t, f'Loai {t}')}: {cnt:,} diem")
    print(f"\n  ==> TONG SO CAMERA CANH BAO: {total_cams:,} camera")
    print(f"  ==> TONG SO BIEN BAO TOAN QUOC: {len(clean_points):,} bien bao")
    print("="*80)
    print(f"HOAN TAT CONVERT! Thu muc chuan da duoc dong goi 100% tai:")
    print(f"  -> {ready_pkg}")

    # 6. Tự động đẩy lên GitHub nếu được yêu cầu
    if auto_push_git:
        publish_to_github(ready_pkg, repo_url=args.repo, branch=args.branch, dry_run=args.dry_run)

if __name__ == '__main__':
    main()
