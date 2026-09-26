"""
Full 3-Source Nationwide Alert Synthesis & SD Card Sync
Sources:
1. VietMap M1 (Data/2/vietmap_edog_2026T9.csv - 38,795 records)
2. Papago GoSafe (Data/1/papago_spc.bin - 25,103 records)
3. SD Card Pre-existing (E:/speedmap/signs.bin.bak - 38,340 records)

Outputs clean, unified binary and CSV databases to:
- E:/speedmap/ (SD Card)
- C:/Users/phamq/radar_car/data/speedmap/ (Firmware Data)
- C:/Users/phamq/Downloads/Data/output_viethud/speedmap/ (Data Package)
"""

import sys
import os
import csv
import struct
import math
import zlib
import time
import shutil
from collections import defaultdict, Counter

sys.stdout.reconfigure(encoding='utf-8')

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

DATA_DIR = r'C:\Users\phamq\Downloads\Data'
M1_CSV = os.path.join(DATA_DIR, '2', 'vietmap_edog_2026T9.csv')
PAPAGO_SPC = os.path.join(DATA_DIR, '1', 'papago_spc.bin')
SD_CARD_SIGNS_BAK = r'E:\speedmap\signs.bin.bak'
SD_CARD_CAMS_BAK = r'E:\speedmap\cameras.bin.bak'

RADAR_CAR_DIR = r'C:\Users\phamq\radar_car\data\speedmap'
OUTPUT_PACKAGE_DIR = os.path.join(DATA_DIR, 'output_viethud', 'speedmap')
DATA2_OUTPUT_DIR = os.path.join(DATA_DIR, '2', 'viethud_output', 'speedmap')
SD_SPEEDMAP_DIR = r'E:\speedmap'

# VietHUD TrafficSignType enum:
SIGN_TYPE_UNKNOWN       = 0
SIGN_TYPE_SPEED_LIMIT   = 1 # P.127: Speed limit
SIGN_TYPE_RESIDENT_AREA = 2 # R.420 / R.421: Khu dong dan cu
SIGN_TYPE_NO_OVERTAKING = 3 # P.125 / DP.133: Cam vuot
SIGN_TYPE_CAMERA        = 4 # Camera phat nguoi
SIGN_TYPE_TOLL_BOOTH    = 5 # P.135: Tram thu phi
SIGN_TYPE_TRAFFIC_LIGHT = 6 # Den giao thong / Camera nga tu
SIGN_TYPE_DANGER_OTHER  = 10 # Cao toc, ham, tram dung, nguy hiem

def main():
    print("="*80)
    print("   VIETHUD - TONG HOP, DONG BO & LAM SACH DU LIEU VOI THE NHO SD CARD")
    print("="*80)

    # 1. Load Source 1: VietMap M1
    print("\n[1/6] Doc du lieu Nguon 1: VietMap M1 (Data/2)...")
    m1_points = []
    with open(M1_CSV, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for r in reader:
            m_type = int(r['ma_loai'])
            speed = int(r['toc_do_kmh'])
            heading = int(r['goc_huong_do'])
            lat = float(r['vi_do'])
            lon = float(r['kinh_do'])
            name = r['ten_loai_canh_bao']
            
            if m_type == 1:
                u_type = SIGN_TYPE_SPEED_LIMIT
                is_camera = True
            elif m_type == 2:
                u_type = SIGN_TYPE_TRAFFIC_LIGHT
                is_camera = True
            elif m_type == 4:
                u_type = SIGN_TYPE_RESIDENT_AREA
                is_camera = False
            elif m_type == 10:
                u_type = SIGN_TYPE_NO_OVERTAKING
                is_camera = False
            elif m_type == 11:
                u_type = SIGN_TYPE_TOLL_BOOTH
                is_camera = False
            else:
                u_type = SIGN_TYPE_DANGER_OTHER
                is_camera = False

            m1_points.append({
                'source': 'VietMap_M1',
                'lat': lat,
                'lon': lon,
                'type': u_type,
                'speed': speed,
                'heading': heading,
                'is_camera': is_camera,
                'desc': name
            })
    print(f"      -> Da nap: {len(m1_points):,} diem canh bao")

    # 2. Load Source 2: Papago GoSafe
    print("\n[2/6] Doc du lieu Nguon 2: Papago GoSafe (Data/1)...")
    with open(PAPAGO_SPC, 'rb') as f:
        f.seek(240)
        t1_records = [f.read(20) for _ in range(11627)]
        f.seek(232780)
        t2_records = [f.read(20) for _ in range(13476)]

    papago_points = []
    for rec in t1_records:
        lon_raw, lat_raw = struct.unpack('<II', rec[:8])
        h_raw = struct.unpack('<H', rec[14:16])[0]
        speed = rec[17]
        heading = (h_raw - 54272) % 360 if h_raw >= 54272 else h_raw
        papago_points.append({
            'source': 'Papago_GoSafe',
            'lat': lat_raw / 1e6,
            'lon': lon_raw / 1e6,
            'type': SIGN_TYPE_SPEED_LIMIT,
            'speed': speed,
            'heading': heading,
            'is_camera': True,
            'desc': f'Camera toc do {speed}km/h (Papago)'
        })

    papago_type_map = {
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
        lon_raw, lat_raw = struct.unpack('<II', rec[:8])
        h_raw = struct.unpack('<H', rec[14:16])[0]
        speed = rec[17]
        p_type = rec[19]
        heading = (h_raw - 54272) % 360 if h_raw >= 54272 else h_raw
        desc_label, sign_type, is_cam = papago_type_map.get(
            p_type, ('Bien bao giao thong', SIGN_TYPE_DANGER_OTHER, False)
        )
        papago_points.append({
            'source': 'Papago_GoSafe',
            'lat': lat_raw / 1e6,
            'lon': lon_raw / 1e6,
            'type': sign_type,
            'speed': speed,
            'heading': heading,
            'is_camera': is_cam,
            'desc': f'{desc_label} (Papago)'
        })
    print(f"      -> Da nap: {len(papago_points):,} diem ({len(t1_records):,} camera + {len(t2_records):,} bien bao)")

    # 3. Load Source 3: SD Card Pre-existing Data
    print(f"\n[3/6] Doc du lieu Nguon 3: Dữ liệu có sẵn trên thẻ nhớ SD ({SD_CARD_SIGNS_BAK})...")
    sd_signs = []
    if os.path.exists(SD_CARD_SIGNS_BAK):
        with open(SD_CARD_SIGNS_BAK, 'rb') as f:
            hdr = f.read(20)
            count = struct.unpack('<HIHII', hdr[4:])[1]
            for _ in range(count):
                data = f.read(20)
                sid, lat_e7, lon_e7, heading, stype, speed, subtype, flags, res = struct.unpack('<IiiHBBBBH', data)
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
                
                sd_signs.append({
                    'source': 'SDCard_WYN',
                    'lat': lat_e7 / 1e7,
                    'lon': lon_e7 / 1e7,
                    'type': stype,
                    'speed': speed,
                    'heading': heading,
                    'is_camera': is_cam,
                    'desc': type_desc_map.get(stype, 'Bien bao (SD)')
                })
        print(f"      -> Da nap: {len(sd_signs):,} diem canh bao tu the nho")
    else:
        print("      -> Khong tim thay file backup tren the nho, bo qua nguon 3")

    # 4. Multi-tier Spatial Hash Dedup and Merge
    print("\n[4/6] Tien hanh HOP NHAT & KHU TRUNG LAP 3 NGUON (Luoi da tang 1m - 12m)...")
    
    GRID_STEP = 0.0001 # ~11m
    unified = list(m1_points)
    grid = defaultdict(list)
    for idx, p in enumerate(unified):
        gx = int(p['lat'] / GRID_STEP)
        gy = int(p['lon'] / GRID_STEP)
        grid[(gx, gy)].append(idx)

    def merge_batch(points_batch, batch_name):
        nonlocal unified, grid
        dups_count = 0
        added_count = 0
        for p in points_batch:
            gx = int(p['lat'] / GRID_STEP)
            gy = int(p['lon'] / GRID_STEP)
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
                    if is_dup:
                        break
                if is_dup:
                    break
                    
            if is_dup:
                dups_count += 1
            else:
                added_count += 1
                unified.append(p)
                grid[(gx, gy)].append(len(unified) - 1)
                
        print(f"      + {batch_name}: Loai bo {dups_count:,} diem trung -> Bo sung {added_count:,} diem doc quyen")

    merge_batch(papago_points, "Papago GoSafe (Nguon 2)")
    if sd_signs:
        merge_batch(sd_signs, "The nho SD Card WYN (Nguon 3)")

    print(f"      -> Tong so diem so bo sau hop nhat 3 nguon: {len(unified):,} diem")

    # ULTRA-FINE SPATIAL GRID PASS (1m - 5m)
    print("      -> Rà soát lưới siêu nhỏ (1m - 5m) để làm sạch triệt để...")
    GRID_1M = 0.000009 # ~1m
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
        if idx in to_drop:
            continue
        gx = int(p['lat'] / GRID_1M)
        gy = int(p['lon'] / GRID_1M)
        
        for dx in range(-5, 6): # ~5m
            for dy in range(-5, 6):
                for o_idx in grid_1m.get((gx + dx, gy + dy), []):
                    if o_idx <= idx or o_idx in to_drop:
                        continue
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
    print(f"      + Khử trùng lặp tuyệt đối (<0.1m): {exact_zero_drop:,} điểm")
    print(f"      + Khử trùng lặp vi mô (1m - 5m):    {micro_drop:,} điểm")
    print(f"      + Bảo toàn đối xứng 2 chiều:        {opp_kept:,} cặp")
    print(f"      + Bảo toàn ngã ba/ngã tư rẽ nhánh:  {cross_kept:,} cặp")
    print(f"      ==> TONG SO DIEM SAU LAM SACH TOAN DIEN: {len(final_clean):,} DIEM")

    # 5. Pack binary data
    print("\n[5/6] Dong goi file nhi phan VietHUD chuan...")
    camera_records = []
    sign_records = []

    cam_id_counter = 8000000000
    sign_id_counter = 1

    for p in final_clean:
        lat_e7 = to_e7(p['lat'])
        lon_e7 = to_e7(p['lon'])
        heading_deg = p['heading'] % 360
        speed = p['speed']
        
        if p['is_camera']:
            cam_records_bytes = struct.pack(
                '<QiihH',  # CameraPoint: int16 speedLimitKmh, THEN uint16 directionDeg (was swapped before 2026-09-26)
                cam_id_counter,
                lat_e7,
                lon_e7,
                speed if speed > 0 else -1,
                heading_deg
            )
            camera_records.append(cam_records_bytes)
            cam_id_counter += 1

        sign_bytes = struct.pack(
            '<IiiHBBBBH',
            sign_id_counter,
            lat_e7,
            lon_e7,
            heading_deg,
            p['type'],
            speed if speed > 0 else 0,
            0, # subType
            0, # flags
            0  # reserved
        )
        sign_records.append(sign_bytes)
        sign_id_counter += 1

    cameras_bin_data = b"".join(camera_records)

    signs_body = b"".join(sign_records)
    signs_crc = zlib.crc32(signs_body) & 0xFFFFFFFF
    signs_header = struct.pack(
        '<4sHIHII',
        b"VNSG",
        1,
        len(sign_records),
        0,
        signs_crc,
        int(time.time())
    )
    signs_bin_data = signs_header + signs_body

    print(f"      -> cameras.bin: {len(camera_records):,} diem camera ({len(cameras_bin_data):,} bytes)")
    print(f"      -> signs.bin:   {len(sign_records):,} bien bao ({len(signs_bin_data):,} bytes, CRC32: 0x{signs_crc:08X})")

    # 6. Synchronize and clean targets
    print("\n[6/6] Dong bo hoa va lam sach the nho SD Card cung cac thu muc dich...")
    
    # Clean obsolete files on SD Card
    if os.path.exists(r'E:\speedmap\tiles'):
        try:
            shutil.rmtree(r'E:\speedmap\tiles')
            print("  [CLEAN] Da xoa thu muc du thua E:\\speedmap\\tiles (V1 leftover)")
        except Exception as e:
            print(f"  [WARN] Khong the xoa E:\\speedmap\\tiles: {e}")

    destinations = [
        ("The nho MicroSD (E:\\speedmap)", SD_SPEEDMAP_DIR),
        ("Thu muc du lieu VietHUD Firmware", RADAR_CAR_DIR),
        ("Goi du lieu xuat tong hop", OUTPUT_PACKAGE_DIR),
        ("Thu muc xuat Data/2", DATA2_OUTPUT_DIR)
    ]

    for desc, dest_dir in destinations:
        if not os.path.exists(os.path.dirname(dest_dir)):
            continue
        os.makedirs(dest_dir, exist_ok=True)
        
        with open(os.path.join(dest_dir, 'cameras.bin'), 'wb') as f:
            f.write(cameras_bin_data)
            
        with open(os.path.join(dest_dir, 'signs.bin'), 'wb') as f:
            f.write(signs_bin_data)
            
        csv_file = os.path.join(dest_dir, 'combined_vietmap_papago_2026T9.csv')
        with open(csv_file, 'w', encoding='utf-8-sig', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['id', 'kinh_do', 'vi_do', 'loai_canh_bao', 'ma_loai', 'toc_do_kmh', 'huong_do', 'la_camera', 'nguon_du_lieu'])
            for idx, p in enumerate(final_clean, start=1):
                writer.writerow([
                    idx,
                    f"{p['lon']:.6f}",
                    f"{p['lat']:.6f}",
                    p['desc'],
                    p['type'],
                    p['speed'],
                    p['heading'],
                    1 if p['is_camera'] else 0,
                    p['source']
                ])

        print(f"  [OK] Da dong bo: {desc} -> {dest_dir}")

    # Copy map files to OUTPUT_PACKAGE_DIR
    map_files = ["metadata.bin", "index.bin", "tiles.bin", "names.bin", "seg_names.bin", "maptiles.bin"]
    for mf in map_files:
        src = os.path.join(RADAR_CAR_DIR, mf)
        if os.path.exists(src):
            dst = os.path.join(OUTPUT_PACKAGE_DIR, mf)
            shutil.copyfile(src, dst)

    sounds_src = os.path.join(RADAR_CAR_DIR, 'sounds')
    sounds_dst = os.path.join(OUTPUT_PACKAGE_DIR, 'sounds')
    if os.path.exists(sounds_src) and not os.path.exists(sounds_dst):
        shutil.copytree(sounds_src, sounds_dst)

    type_counts = Counter(p['type'] for p in final_clean)
    type_names = {
        SIGN_TYPE_SPEED_LIMIT: 'Camera bắn tốc độ / Giới hạn tốc độ (Type 1)',
        SIGN_TYPE_RESIDENT_AREA: 'Khu đông dân cư Vào/Ra (Type 2)',
        SIGN_TYPE_NO_OVERTAKING: 'Đoạn đường cấm vượt (Type 3)',
        SIGN_TYPE_CAMERA: 'Camera phạt nguội độc lập (Type 4)',
        SIGN_TYPE_TOLL_BOOTH: 'Trạm thu phí BOT (Type 5)',
        SIGN_TYPE_TRAFFIC_LIGHT: 'Đèn giao thông / Camera vượt đèn đỏ (Type 6)',
        SIGN_TYPE_DANGER_OTHER: 'Lối vào cao tốc / Hầm / Trạm dừng / Cầu hẹp (Type 10)'
    }

    print("\n" + "="*80)
    print("THONG KE TONG HOP TOAN QUOC (3 NGUON: VIETMAP + PAPAGO + THE NHO SD):")
    print("="*80)
    for t, cnt in sorted(type_counts.items()):
        print(f"  * {type_names.get(t, f'Loai {t}')}: {cnt:,} diem")

    total_cams = sum(1 for p in final_clean if p['is_camera'])
    print(f"\n  ==> TONG SO CAMERA KICH HOAT CANH BAO: {total_cams:,} camera")
    print(f"  ==> TONG SO BIEN BAO GIAO THONG:        {len(final_clean):,} bien bao")
    print("="*80)
    print("HOAN TAT! The nho SD Card va toan bo he thong da duoc dong bo va lam sach.")
    print("="*80)

if __name__ == '__main__':
    main()
