import csv
import math
import struct
import os

HANOI_LAT = 21.0285
HANOI_LON = 105.8542
MAX_DIST_KM = 200.0

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
                        'speed': speed
                    })
            except Exception:
                continue

    # Sort alerts by latitude
    alerts.sort(key=lambda a: (a['latE7'], a['lonE7']))
    total = len(alerts)
    print(f"Total alerts within 200km Hanoi: {total}")

    # Generate compressed C++ source files
    out_cpp = '/home/admin/HanoiAlertsData.cpp'
    out_h = '/home/admin/HanoiAlertsData.h'

    with open(out_h, 'w', encoding='utf-8') as fh:
        fh.write('#pragma once\n#include <Arduino.h>\n\n')
        fh.write('#pragma pack(push, 1)\n')
        fh.write('struct HanoiAlertRecord {\n')
        fh.write('    int32_t latE7;\n')
        fh.write('    int32_t lonE7;\n')
        fh.write('    uint16_t heading;\n')
        fh.write('    uint8_t type;\n')
        fh.write('    uint8_t speed;\n')
        fh.write('};\n')
        fh.write('#pragma pack(pop)\n\n')
        fh.write(f'#define HANOI_ALERT_COUNT {total}\n')
        fh.write('extern const HanoiAlertRecord HANOI_ALERTS[] PROGMEM;\n')

    with open(out_cpp, 'w', encoding='utf-8') as fc:
        fc.write('#include "HanoiAlertsData.h"\n\n')
        fc.write('const HanoiAlertRecord HANOI_ALERTS[] PROGMEM = {\n')
        for i, a in enumerate(alerts):
            fc.write(f"{{{a['latE7']},{a['lonE7']},{a['heading']},{a['type']},{a['speed']}}},\n")
            if i % 1000 == 0:
                print(f"Written {i}/{total}...")
        fc.write('};\n')

    print(f"Generated {out_h} and {out_cpp} ({os.path.getsize(out_cpp)/1024/1024:.2f} MB)")

if __name__ == '__main__':
    main()
