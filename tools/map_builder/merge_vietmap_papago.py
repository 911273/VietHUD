"""
Merges VietMap M1 (38,795 points) and Papago GoSafe 2026T9 (25,103 points)
into a unified, deduplicated, maximum-coverage traffic alert dataset.

Outputs:
1. data/speedmap/combined_vietmap_papago_2026T9.csv
2. data/speedmap/cameras.bin (CameraPoint format for firmware)
3. data/speedmap/signs.bin (VNSG TrafficSignPoint format for firmware)
"""

import sys
import os
import csv
import struct
import math
import zlib
import time
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

def to_e7(val):
    return int(round(val * 1e7))

M1_CSV = r'c:\Users\phamq\Downloads\VietMap_SpeedMap_M1_2026T9\vietmap_edog_2026T9.csv'
PAPAGO_SPC = r'C:\Users\phamq\Downloads\Papago_GoSafe51G_GosafeS70G_2026T9\Papago_GoSafe51G_GosafeS70G_2026T9\papago_spc.bin'
OUT_DIR = r'C:\Users\phamq\radar_car\data\speedmap'

# TrafficSignType constants matching SpeedMapFormat.h:
SIGN_TYPE_UNKNOWN       = 0
SIGN_TYPE_SPEED_LIMIT   = 1 # P.127
SIGN_TYPE_RESIDENT_AREA = 2 # R.420 / R.421
SIGN_TYPE_NO_OVERTAKING = 3 # P.125 / DP.133
SIGN_TYPE_CAMERA        = 4 # Camera phạt nguội
SIGN_TYPE_TOLL_BOOTH    = 5 # P.135
SIGN_TYPE_TRAFFIC_LIGHT = 6 # Đèn giao thông / ngã tư
SIGN_TYPE_DANGER_OTHER  = 10 # Cao tốc, hầm, trạm dừng, nguy hiểm

print(f"=== HỢP NHẤT DỮ LIỆU VIETMAP M1 & PAPAGO GOSAFE 2026T9 ===")

# 1. Load M1 dataset
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
        
        # M1 mapping:
        # 1: Camera bắn tốc độ -> SIGN_TYPE_SPEED_LIMIT / Camera
        # 2: Camera phạt nguội / Vượt đèn đỏ -> SIGN_TYPE_TRAFFIC_LIGHT / Camera
        # 4: Khu dân cư -> SIGN_TYPE_RESIDENT_AREA
        # 10: Cấm vượt -> SIGN_TYPE_NO_OVERTAKING
        # 11: Trạm thu phí -> SIGN_TYPE_TOLL_BOOTH
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

print(f"[1/4] Đã tải VietMap M1: {len(m1_points):,} điểm")

# 2. Build spatial hash grid on M1 points (grid cell ~500m)
GRID = 0.005
unified_list = list(m1_points)
grid = defaultdict(list)
for idx, p in enumerate(unified_list):
    gx = int(p['lat'] / GRID)
    gy = int(p['lon'] / GRID)
    grid[(gx, gy)].append(idx)

# 3. Load and parse Papago SPC
with open(PAPAGO_SPC, 'rb') as f:
    # Table 1: 11,627 speed cameras (Offset 240)
    f.seek(240)
    t1_records = [f.read(20) for _ in range(11627)]
    # Table 2: 13,476 traffic signs (Offset 232,780)
    f.seek(232780)
    t2_records = [f.read(20) for _ in range(13476)]

print(f"[2/4] Đã nạp Papago spc.bin: {len(t1_records):,} camera Bảng 1 + {len(t2_records):,} biển báo Bảng 2")

papago_parsed = []
for rec in t1_records:
    lon_raw, lat_raw = struct.unpack('<II', rec[:8])
    h_raw = struct.unpack('<H', rec[14:16])[0]
    speed = rec[17]
    heading = (h_raw - 54272) % 360 if h_raw >= 54272 else h_raw
    papago_parsed.append({
        'lat': lat_raw / 1e6,
        'lon': lon_raw / 1e6,
        'type': SIGN_TYPE_SPEED_LIMIT,
        'speed': speed,
        'heading': heading,
        'is_camera': True,
        'desc': f'Camera tốc độ {speed}km/h (Papago)'
    })

name_map_papago = {
    2: ('Khu dân cư (Vào/Ra)', SIGN_TYPE_RESIDENT_AREA, False),
    3: ('Cấm vượt / Hết cấm vượt', SIGN_TYPE_NO_OVERTAKING, False),
    4: ('Lối vào cao tốc / Nút giao', SIGN_TYPE_DANGER_OTHER, False),
    5: ('Trạm thu phí BOT', SIGN_TYPE_TOLL_BOOTH, False),
    6: ('Đèn tín hiệu giao thông / Phạt ngã tư', SIGN_TYPE_TRAFFIC_LIGHT, True),
    7: ('Đường hầm / Cầu vượt', SIGN_TYPE_DANGER_OTHER, False),
    8: ('Trạm dừng nghỉ', SIGN_TYPE_DANGER_OTHER, False),
    9: ('Cầu hẹp / Đoạn nguy hiểm', SIGN_TYPE_DANGER_OTHER, False)
}

for rec in t2_records:
    lon_raw, lat_raw = struct.unpack('<II', rec[:8])
    h_raw = struct.unpack('<H', rec[14:16])[0]
    speed = rec[17]
    p_type = rec[19]
    heading = (h_raw - 54272) % 360 if h_raw >= 54272 else h_raw
    
    desc_label, sign_type, is_cam = name_map_papago.get(p_type, ('Biển báo giao thông', SIGN_TYPE_DANGER_OTHER, False))
    papago_parsed.append({
        'lat': lat_raw / 1e6,
        'lon': lon_raw / 1e6,
        'type': sign_type,
        'speed': speed,
        'heading': heading,
        'is_camera': is_cam,
        'desc': f'{desc_label} (Papago)'
    })

# 4. Deduplicate and merge Papago into Unified
skipped_dups = 0
added_papago = 0

for p in papago_parsed:
    gx = int(p['lat'] / GRID)
    gy = int(p['lon'] / GRID)
    is_dup = False
    
    # Check 9 surrounding cells
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for u_idx in grid.get((gx + dx, gy + dy), []):
                u = unified_list[u_idx]
                dist = haversine_m(p['lat'], p['lon'], u['lat'], u['lon'])
                if dist <= 38.0:
                    # Same category or camera type
                    if p['type'] == u['type'] or (p['is_camera'] and u['is_camera']):
                        is_dup = True
                        # If M1 point lacked speed limit but Papago has it, enrich M1
                        if u['speed'] == 0 and p['speed'] > 0:
                            u['speed'] = p['speed']
                        break
            if is_dup:
                break
        if is_dup:
            break

    if is_dup:
        skipped_dups += 1
    else:
        added_papago += 1
        new_entry = {
            'source': 'Papago_GoSafe',
            'lat': p['lat'],
            'lon': p['lon'],
            'type': p['type'],
            'speed': p['speed'],
            'heading': p['heading'],
            'is_camera': p['is_camera'],
            'desc': p['desc']
        }
        unified_list.append(new_entry)
        grid[(gx, gy)].append(len(unified_list) - 1)

print(f"[3/4] Khử trùng lặp không gian (bán kính 38m): Bỏ qua {skipped_dups:,} điểm trùng lặp, thêm thành công {added_papago:,} điểm độc nhất từ Papago!")
print(f"       ==> TỔNG SỐ ĐIỂM SAU HỢP NHẤT: {len(unified_list):,} điểm cảnh báo")

# 5. Export to Unified CSV
combined_csv_path = os.path.join(OUT_DIR, "combined_vietmap_papago_2026T9.csv")
with open(combined_csv_path, 'w', encoding='utf-8-sig', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['id', 'kinh_do', 'vi_do', 'loai_canh_bao', 'ma_loai', 'toc_do_kmh', 'huong_do', 'la_camera', 'nguon_du_lieu'])
    for idx, p in enumerate(unified_list, start=1):
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
print(f"[4/4] Đã xuất file CSV tổng hợp: {combined_csv_path} ({len(unified_list):,} dòng)")

# 6. Build cameras.bin and signs.bin for ESP32 firmware
# CameraPoint struct: uint64_t id, int32_t latE7, int32_t lonE7, int16_t speedLimitKmh, uint16_t directionDeg (20 bytes)
# TrafficSignPoint struct: uint32_t id, int32_t latE7, int32_t lonE7, uint16_t directionDeg, uint8_t signType, uint8_t speedLimitKmh, uint8_t subType, uint8_t flags, uint16_t reserved (20 bytes)

camera_records = []
sign_records = []

cam_id_counter = 8000000000
sign_id_counter = 1

for p in unified_list:
    lat_e7 = to_e7(p['lat'])
    lon_e7 = to_e7(p['lon'])
    heading_deg = p['heading'] % 360
    speed = p['speed']
    
    # Every camera point goes to cameras.bin
    if p['is_camera']:
        cam_records_bytes = struct.pack(
            '<QiiHh',
            cam_id_counter,
            lat_e7,
            lon_e7,
            heading_deg,
            speed if speed > 0 else -1
        )
        camera_records.append(cam_records_bytes)
        cam_id_counter += 1

    # Every sign point goes to signs.bin (including cameras as visual HUD sign markers)
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

# Write cameras.bin
cameras_bin_path = os.path.join(OUT_DIR, "cameras.bin")
with open(cameras_bin_path, 'wb') as f:
    for rec in camera_records:
        f.write(rec)

print(f"      -> cameras.bin: {len(camera_records):,} điểm camera ({os.path.getsize(cameras_bin_path):,} bytes)")

# Write signs.bin with VNSG Header
# TrafficSignHeader: char magic[4], uint16_t version, uint32_t signCount, uint16_t reserved, uint32_t crc32, uint32_t timestamp (20 bytes)
signs_body = b"".join(sign_records)
signs_crc = zlib.crc32(signs_body) & 0xFFFFFFFF
header = struct.pack(
    '<4sHIHII',
    b"VNSG",
    1, # version 1
    len(sign_records),
    0, # reserved
    signs_crc,
    int(time.time())
)

signs_bin_path = os.path.join(OUT_DIR, "signs.bin")
with open(signs_bin_path, 'wb') as f:
    f.write(header)
    f.write(signs_body)

print(f"      -> signs.bin: {len(sign_records):,} biển báo giao thông ({os.path.getsize(signs_bin_path):,} bytes, CRC: 0x{signs_crc:08X})")

# Summary stats
type_counts = Counter(p['type'] for p in unified_list)
type_names = {
    SIGN_TYPE_SPEED_LIMIT: 'Camera bắn tốc độ (Type 1)',
    SIGN_TYPE_RESIDENT_AREA: 'Khu đông dân cư (Type 2)',
    SIGN_TYPE_NO_OVERTAKING: 'Đoạn đường cấm vượt (Type 3)',
    SIGN_TYPE_TOLL_BOOTH: 'Trạm thu phí BOT (Type 5)',
    SIGN_TYPE_TRAFFIC_LIGHT: 'Đèn giao thông / Camera vượt đèn đỏ (Type 6)',
    SIGN_TYPE_DANGER_OTHER: 'Lối vào cao tốc / Hầm / Trạm dừng / Cầu hẹp (Type 10)'
}

print("\n--- PHÂN BỔ LOẠI CẢNH BÁO SAU HỢP NHẤT ---")
for t, cnt in sorted(type_counts.items()):
    print(f"  * {type_names.get(t, f'Loại {t}')}: {cnt:,} điểm")

print("\n=== HOÀN TẤT HỢP NHẤT DỮ LIỆU THÀNH CÔNG! ===")
