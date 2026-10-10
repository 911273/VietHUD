import csv
import math
import struct
import json

HANOI_LAT = 21.0285
HANOI_LON = 105.8542
MAX_DIST_KM = 200.0

def haversine(lat1, lon1, lat2, lon2):
    R = 6371.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlambda/2)**2
    return 2 * R * math.atan2(math.sqrt(a), math.sqrt(1 - a))

def main():
    csv_path = '/home/admin/viethud-pipeline/output/speedmap/combined_traffic_alerts.csv'
    alerts = []
    type_counts = {}

    with open(csv_path, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                lon = float(row['kinh_do'])
                lat = float(row['vi_do'])
                d = haversine(HANOI_LAT, HANOI_LON, lat, lon)
                if d <= MAX_DIST_KM:
                    m_type = int(row.get('ma_loai', 0))
                    speed = int(float(row.get('toc_do_kmh', 0)))
                    heading = int(float(row.get('huong_do', 0))) % 360
                    is_cam = int(row.get('la_camera', 0))
                    
                    alerts.append({
                        'lat': lat,
                        'lon': lon,
                        'type': m_type,
                        'speed': speed,
                        'heading': heading,
                        'is_cam': is_cam,
                        'dist_hanoi': round(d, 2)
                    })
                    type_counts[m_type] = type_counts.get(m_type, 0) + 1
            except Exception:
                continue

    print(f"Total Hanoi 200km alerts: {len(alerts)}")
    print("Type distribution:")
    for k, v in sorted(type_counts.items()):
        print(f"  Type {k}: {v} alerts")

if __name__ == '__main__':
    main()
