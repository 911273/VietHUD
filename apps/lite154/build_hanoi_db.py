import csv
import math
import struct
import os

HANOI_LAT = 21.0285
HANOI_LON = 105.8542
MAX_DIST_KM = 200.0

# Alert Types matching VietHUD standard
SIGN_TYPE_UNKNOWN       = 0
SIGN_TYPE_SPEED_LIMIT   = 1 # Camera bắn tốc độ / P.127: Giới hạn tốc độ
SIGN_TYPE_RESIDENT_AREA = 2 # R.420 / R.421: Khu đông dân cư
SIGN_TYPE_NO_OVERTAKING = 3 # P.125 / DP.133: Đoạn đường cấm vượt
SIGN_TYPE_CAMERA        = 4 # Camera phạt nguội
SIGN_TYPE_TOLL_BOOTH    = 5 # P.135: Trạm thu phí BOT
SIGN_TYPE_TRAFFIC_LIGHT = 6 # Đèn tín hiệu giao thông / Camera vượt đèn đỏ
SIGN_TYPE_DANGER_OTHER  = 10 # Đường hầm, cầu hẹp, nguy hiểm khác

def haversine(lat1, lon1, lat2, lon2):
    R = 6371.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlambda/2)**2
    return 2 * R * math.atan2(math.sqrt(a), math.sqrt(1 - a))

def main():
    csv_path = '/home/admin/viethud-pipeline/output/speedmap/combined_traffic_alerts.csv'
    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found")
        return

    alerts = []
    type_stats = {}

    with open(csv_path, 'r', encoding='utf-8-sig', errors='ignore') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                lon = float(row['kinh_do'])
                lat = float(row['vi_do'])
                dist = haversine(HANOI_LAT, HANOI_LON, lat, lon)
                if dist <= MAX_DIST_KM:
                    m_type = int(row.get('ma_loai', 0))
                    speed = int(float(row.get('toc_do_kmh', 0)))
                    heading = int(float(row.get('huong_do', 0))) % 360
                    is_cam = int(row.get('la_camera', 0))

                    # Map type to standardized VietHUD sign types
                    if m_type == 1:
                        final_type = SIGN_TYPE_SPEED_LIMIT
                    elif m_type == 2:
                        final_type = SIGN_TYPE_TRAFFIC_LIGHT
                    elif m_type == 4:
                        final_type = SIGN_TYPE_RESIDENT_AREA
                    elif m_type == 5:
                        final_type = SIGN_TYPE_NO_OVERTAKING
                    elif m_type == 6:
                        final_type = SIGN_TYPE_TOLL_BOOTH
                    elif m_type == 10:
                        final_type = SIGN_TYPE_DANGER_OTHER
                    else:
                        final_type = SIGN_TYPE_CAMERA if is_cam else SIGN_TYPE_UNKNOWN

                    alerts.append({
                        'latE7': int(round(lat * 1e7)),
                        'lonE7': int(round(lon * 1e7)),
                        'heading': heading,
                        'type': final_type,
                        'speed': speed,
                        'is_cam': is_cam,
                        'dist': dist
                    })
                    type_stats[final_type] = type_stats.get(final_type, 0) + 1
            except Exception:
                continue

    # Sort alerts by latitude for fast spatial indexing / binary search
    alerts.sort(key=lambda a: (a['latE7'], a['lonE7']))

    total = len(alerts)
    print(f"Successfully processed {total} alerts within 200km of Hanoi.")
    print("Statistics by Category:")
    print(f"  - Speed Cameras / Speed Limits (Type 1): {type_stats.get(SIGN_TYPE_SPEED_LIMIT, 0)}")
    print(f"  - Traffic Lights / Red Light Cameras (Type 6): {type_stats.get(SIGN_TYPE_TRAFFIC_LIGHT, 0)}")
    print(f"  - Populated Areas - Khu dan cu (Type 2): {type_stats.get(SIGN_TYPE_RESIDENT_AREA, 0)}")
    print(f"  - No Overtaking - Cam vuot (Type 3): {type_stats.get(SIGN_TYPE_NO_OVERTAKING, 0)}")
    print(f"  - Toll Booths - Tram thu phi BOT (Type 5): {type_stats.get(SIGN_TYPE_TOLL_BOOTH, 0)}")
    print(f"  - Independent Cameras (Type 4): {type_stats.get(SIGN_TYPE_CAMERA, 0)}")
    print(f"  - Highway / Tunnel / Danger Zones (Type 10): {type_stats.get(SIGN_TYPE_DANGER_OTHER, 0)}")

    # Pack into binary format:
    # Header: Magic "VNHN" (4B), Version 1 (2B), Count (4B), CenterLatE7 (4B), CenterLonE7 (4B), RadiusKm (2B) = 20 Bytes
    # Each record: latE7 (4B), lonE7 (4B), heading (2B), type (1B), speed (1B) = 12 Bytes
    out_bin = '/var/www/viethud/hanoi_alerts.bin'
    header = struct.pack('<4sHIIIH', b'VNHN', 1, total, int(round(HANOI_LAT * 1e7)), int(round(HANOI_LON * 1e7)), int(MAX_DIST_KM))

    with open(out_bin, 'wb') as f:
        f.write(header)
        for a in alerts:
            rec = struct.pack('<iiHBB', a['latE7'], a['lonE7'], a['heading'], a['type'], a['speed'])
            f.write(rec)

    file_size = os.path.getsize(out_bin)
    print(f"Saved binary database to {out_bin} ({file_size} bytes, {file_size/1024:.1f} KB)")

if __name__ == '__main__':
    main()
