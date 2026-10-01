#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VietHUD SpeedMap Studio - Backend Server (V5.3 One-Click Permanent Persistence)
"""

import os
import sys
import json
import math
import struct
import csv
import time
import zlib
import gzip
import shutil
from pathlib import Path
from http.server import HTTPServer, SimpleHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse, parse_qs

PORT = 8088
BASE_DIR = Path(__file__).resolve().parent
DEFAULT_SPEEDMAP = Path(r"C:\Users\phamq\radar_car\speedmap")
DEFAULT_CSV = Path(r"c:\Users\phamq\Downloads\cameras.csv")

CAMERA_CATEGORIES = {"speed_cam", "red_light_cam"}
SIGN_CATEGORIES = {"sign_speed", "sign_r420", "sign_r421", "sign_overtake", "traffic_light", "toll", "danger"}

TYPE_MAP_SIGNS = {
    "sign_speed": 1,
    "sign_r420": 2,
    "sign_r421": 3,
    "sign_overtake": 7,
    "traffic_light": 6,
    "toll": 5,
    "danger": 10
}

def dist_point_to_seg(plat, plon, s_lat, s_lon, e_lat, e_lon):
    m_lat = 110540.0
    m_lon = 111320.0 * math.cos(math.radians(plat))
    px = (plon - s_lon) * m_lon
    py = (plat - s_lat) * m_lat
    dx = (e_lon - s_lon) * m_lon
    dy = (e_lat - s_lat) * m_lat
    l2 = dx*dx + dy*dy
    if l2 < 1e-6:
        return math.sqrt(px*px + py*py), s_lat, s_lon, 0.0
    t = max(0.0, min(1.0, (px*dx + py*dy) / l2))
    proj_x = t * dx
    proj_y = t * dy
    d = math.sqrt((px - proj_x)**2 + (py - proj_y)**2)
    seg_heading = math.degrees(math.atan2(dx, dy)) % 360
    proj_lat = s_lat + t * (e_lat - s_lat)
    proj_lon = s_lon + t * (e_lon - s_lon)
    return d, proj_lat, proj_lon, seg_heading

class SpatialGrid:
    def __init__(self, cell_size=0.04):
        self.cell_size = cell_size
        self.grid = {}

    def clear(self):
        self.grid.clear()

    def add(self, item):
        c_lat = int(math.floor(item["lat"] / self.cell_size))
        c_lon = int(math.floor(item["lon"] / self.cell_size))
        key = (c_lat, c_lon)
        if key not in self.grid:
            self.grid[key] = []
        self.grid[key].append(item)

    def remove(self, item_id):
        for bucket in self.grid.values():
            for i, it in enumerate(bucket):
                if it.get("id") == item_id:
                    bucket.pop(i)
                    return True
        return False

    def query(self, min_lat, max_lat, min_lon, max_lon, limit=4000):
        min_c_lat = int(math.floor(min_lat / self.cell_size))
        max_c_lat = int(math.floor(max_lat / self.cell_size))
        min_c_lon = int(math.floor(min_lon / self.cell_size))
        max_c_lon = int(math.floor(max_lon / self.cell_size))
        
        results = []
        for c_lat in range(min_c_lat, max_c_lat + 1):
            for c_lon in range(min_c_lon, max_c_lon + 1):
                bucket = self.grid.get((c_lat, c_lon))
                if not bucket:
                    continue
                for item in bucket:
                    la = item["lat"]
                    lo = item["lon"]
                    if min_lat <= la <= max_lat and min_lon <= lo <= max_lon:
                        results.append(item)
                        if len(results) >= limit:
                            return results
        return results

class SpeedMapEngine:
    def __init__(self):
        self.road_files = {
            "tiles": DEFAULT_SPEEDMAP / "tiles.bin",
            "index": DEFAULT_SPEEDMAP / "index.bin",
            "names": DEFAULT_SPEEDMAP / "names.bin",
            "seg_names": DEFAULT_SPEEDMAP / "seg_names.bin"
        }
        self.tile_entries = {}
        self.tile_size = 0.01
        self.names_cache = {}
        self.f_tiles = None
        self.f_seg_names = None
        self.tile_decoded_cache = {}

        self.active_camera_file = str(DEFAULT_CSV if DEFAULT_CSV.exists() else (DEFAULT_SPEEDMAP / "cameras.bin"))
        self.camera_records = []
        self.camera_records_map = {}
        self.camera_grid = SpatialGrid(cell_size=0.03)
        self.next_cam_id = 1000000

        self.active_sign_file = str(DEFAULT_SPEEDMAP / "signs.bin")
        self.sign_records = []
        self.sign_records_map = {}
        self.sign_grid = SpatialGrid(cell_size=0.03)
        self.next_sign_id = 500000

        self.changes_count = 0

        self.load_roads()
        self.load_cameras(self.active_camera_file)
        self.load_signs(self.active_sign_file)

    def load_roads(self):
        idx_path = Path(self.road_files["index"])
        tiles_path = Path(self.road_files["tiles"])
        names_path = Path(self.road_files["names"])
        seg_names_path = Path(self.road_files["seg_names"])

        print(f"[*] Loading Road Network from: {tiles_path.name}")
        self.tile_decoded_cache.clear()

        if idx_path.exists():
            idx_bytes = idx_path.read_bytes()
            n_tiles = len(idx_bytes) // 28
            self.tile_entries = {}
            for i in range(n_tiles):
                tid, mnLa, mxLa, mnLo, mxLo, off, sz = struct.unpack('<IiiiiII', idx_bytes[i*28:(i+1)*28])
                self.tile_entries[tid] = (off, sz)
            print(f"  + Loaded {len(self.tile_entries):,} map tiles from index.bin")
        else:
            self.tile_entries = {}

        if self.f_tiles:
            try: self.f_tiles.close()
            except: pass
        if tiles_path.exists():
            self.f_tiles = open(tiles_path, "rb")

        # 1-BASED INDEX FOR STREET NAMES
        self.names_cache = {}
        if names_path.exists():
            try:
                nb = names_path.read_bytes()
                if len(nb) >= 16 and nb[:4] == b"VNNM":
                    magic, ver, count, pool_bytes, res = struct.unpack("<4sHIIH", nb[:16])
                    offsets = struct.unpack(f"<{count}I", nb[16 : 16 + count * 4])
                    pool = nb[16 + count * 4 :]
                    for i in range(count):
                        off = offsets[i]
                        end = pool.find(b"\x00", off)
                        if end == -1: end = len(pool)
                        name_str = pool[off:end].decode("utf-8", errors="replace")
                        self.names_cache[i + 1] = name_str
                    print(f"  + Loaded {len(self.names_cache):,} street names into 1-based index")
            except Exception as e:
                print(f"  ! Error reading names: {e}")

        if self.f_seg_names:
            try: self.f_seg_names.close()
            except: pass
        if seg_names_path.exists():
            self.f_seg_names = open(seg_names_path, "rb")

    def load_cameras(self, path_str: str):
        p = Path(path_str)
        if not p.exists():
            return
        self.active_camera_file = str(p)
        records = []
        self.camera_grid.clear()

        if p.suffix.lower() == ".bin":
            data = p.read_bytes()
            cnt = len(data) // 20
            for i in range(cnt):
                osm_id, lat_e7, lon_e7, speed, heading = struct.unpack("<QiihH", data[i*20 : (i+1)*20])
                is_speed = speed > 0
                rec = {
                    "id": osm_id,
                    "lat": round(lat_e7 / 1e7, 7),
                    "lon": round(lon_e7 / 1e7, 7),
                    "speed": speed,
                    "heading": heading,
                    "category": "speed_cam" if is_speed else "red_light_cam",
                    "desc": f"Camera tốc độ ({speed} km/h)" if is_speed else "Camera phạt nguội / đèn đỏ",
                    "modified": False
                }
                records.append(rec)
                self.camera_grid.add(rec)
        else: # CSV
            with open(p, "r", encoding="utf-8-sig") as f:
                reader = csv.DictReader(f)
                for idx, r in enumerate(reader):
                    try:
                        la = float(r.get("lat", 0))
                        lo = float(r.get("lon", 0))
                        spd = int(float(r.get("speed_limit", 0)))
                        hd = int(float(r.get("heading_deg", 0)))
                        stype = r.get("type_code", r.get("sign_type", ""))
                        desc = r.get("description", r.get("sign_name", ""))
                        is_speed = spd > 0 and "SPEED" in stype.upper()
                        cat = "speed_cam" if is_speed else "red_light_cam"
                        rec = {
                            "id": idx + 1,
                            "lat": round(la, 7),
                            "lon": round(lo, 7),
                            "speed": spd,
                            "heading": hd,
                            "category": cat,
                            "desc": desc,
                            "modified": False
                        }
                        records.append(rec)
                        self.camera_grid.add(rec)
                    except Exception:
                        continue
        self.camera_records = records
        self.camera_records_map = {r["id"]: r for r in records}
        print(f"  + Indexed {len(records):,} cameras into spatial grid")

    def load_signs(self, path_str: str):
        p = Path(path_str)
        if not p.exists():
            return
        self.active_sign_file = str(p)
        records = []
        self.sign_grid.clear()

        if p.suffix.lower() == ".bin":
            s_data = p.read_bytes()
            if len(s_data) >= 20 and s_data[:4] == b"VNSG":
                magic, ver, count, res, crc_s, crc_b = struct.unpack("<4sHIHII", s_data[:20])
                for i in range(count):
                    raw = s_data[20 + i*20 : 20 + (i+1)*20]
                    seg_id, lat_e7, lon_e7, heading, stype, val, fl1, fl2, dist = struct.unpack("<IiiHBBBBH", raw)
                    
                    if stype == 1: cat = "sign_speed"
                    elif stype == 2: cat = "sign_r420"
                    elif stype == 3: cat = "sign_r421"
                    elif stype in (7, 8): cat = "sign_overtake"
                    elif stype == 6: cat = "traffic_light"
                    elif stype == 5: cat = "toll"
                    else: cat = "danger"

                    rec = {
                        "id": i + 1,
                        "seg_id": seg_id,
                        "lat": round(lat_e7 / 1e7, 7),
                        "lon": round(lon_e7 / 1e7, 7),
                        "heading": heading,
                        "type": stype,
                        "category": cat,
                        "value": val,
                        "flags": fl1,
                        "dist": dist,
                        "desc": "Đèn giao thông" if stype == 6 else f"Biển báo ({cat})",
                        "modified": False
                    }
                    records.append(rec)
                    self.sign_grid.add(rec)
        else: # CSV
            with open(p, "r", encoding="utf-8-sig") as f:
                reader = csv.DictReader(f)
                for idx, r in enumerate(reader):
                    try:
                        stype = int(r.get("sign_type", 1))
                        if stype == 1: cat = "sign_speed"
                        elif stype == 2: cat = "sign_r420"
                        elif stype == 3: cat = "sign_r421"
                        elif stype in (7, 8): cat = "sign_overtake"
                        elif stype == 6: cat = "traffic_light"
                        elif stype == 5: cat = "toll"
                        else: cat = "danger"

                        rec = {
                            "id": idx + 1,
                            "seg_id": int(r.get("segment_id", 0)),
                            "lat": round(float(r.get("lat", 0)), 7),
                            "lon": round(float(r.get("lon", 0)), 7),
                            "heading": int(float(r.get("heading", 0))),
                            "type": stype,
                            "category": cat,
                            "value": int(float(r.get("value", 0))),
                            "flags": 0,
                            "dist": 0,
                            "desc": r.get("description", "Đèn giao thông" if stype == 6 else f"Biển báo ({cat})"),
                            "modified": False
                        }
                        records.append(rec)
                        self.sign_grid.add(rec)
                    except Exception:
                        continue
        self.sign_records = records
        self.sign_records_map = {r["id"]: r for r in records}
        print(f"  + Indexed {len(records):,} signs into spatial grid")

    def get_road_name(self, seg_id):
        if not self.f_seg_names or seg_id <= 0:
            return ""
        try:
            self.f_seg_names.seek(seg_id * 4)
            b = self.f_seg_names.read(4)
            if len(b) == 4:
                name_id = struct.unpack("<I", b)[0]
                if name_id <= 0:
                    return ""
                return self.names_cache.get(name_id, "")
        except:
            pass
        return ""

    def get_tile_segments(self, tid):
        if tid in self.tile_decoded_cache:
            return self.tile_decoded_cache[tid]
        
        t_ent = self.tile_entries.get(tid)
        if not t_ent or not self.f_tiles:
            return None

        off, sz = t_ent
        self.f_tiles.seek(off)
        t_bytes = self.f_tiles.read(sz)
        n_segs = len(t_bytes) // 28
        segs = []
        for s_i in range(n_segs):
            sid, sLa, sLo, eLa, eLo, hdeg, rcls, rdir, rlim, rsrc, rflg = struct.unpack(
                '<IiiiiHBBhBB', t_bytes[s_i*28:(s_i+1)*28]
            )
            segs.append((sid, sLa/1e7, sLo/1e7, eLa/1e7, eLo/1e7, hdeg, rcls, rdir, rlim, rflg))

        if len(self.tile_decoded_cache) > 400:
            self.tile_decoded_cache.clear()
        self.tile_decoded_cache[tid] = segs
        return segs

    def query_roads(self, min_lat, max_lat, min_lon, max_lon, zoom=15):
        if not self.f_tiles:
            return []

        if zoom <= 12:
            max_road_class = 1
            max_returned = 1500
        elif zoom <= 13:
            max_road_class = 2
            max_returned = 2500
        elif zoom <= 14:
            max_road_class = 3
            max_returned = 4000
        else:
            max_road_class = 5
            max_returned = 6000

        lat_min_cell = int((min_lat + 90.0) / self.tile_size)
        lat_max_cell = int((max_lat + 90.0) / self.tile_size)
        lon_min_cell = int((min_lon + 180.0) / self.tile_size)
        lon_max_cell = int((max_lon + 180.0) / self.tile_size)

        segments = []

        for lat_c in range(lat_min_cell, lat_max_cell + 1):
            for lon_c in range(lon_min_cell, lon_max_cell + 1):
                tid = (lat_c << 16) | (lon_c & 0xFFFF)
                segs = self.get_tile_segments(tid)
                if not segs:
                    continue

                for sid, slat, slon, elat, elon, hdeg, rcls, rdir, rlim, rflg in segs:
                    if rcls > max_road_class:
                        continue
                    if (max(slat, elat) < min_lat or min(slat, elat) > max_lat or
                        max(slon, elon) < min_lon or min(slon, elon) > max_lon):
                        continue

                    rname = self.get_road_name(sid) if zoom >= 13 else ""
                    segments.append({
                        "id": sid,
                        "start": [round(slat, 6), round(slon, 6)],
                        "end": [round(elat, 6), round(elon, 6)],
                        "heading": hdeg,
                        "road_class": rcls,
                        "speed_limit": rlim,
                        "direction": rdir,
                        "flags": rflg,
                        "name": rname
                    })

                    if len(segments) >= max_returned:
                        return segments
        return segments

    def query_cameras(self, min_lat, max_lat, min_lon, max_lon):
        return self.camera_grid.query(min_lat, max_lat, min_lon, max_lon, limit=2500)

    def query_signs(self, min_lat, max_lat, min_lon, max_lon):
        return self.sign_grid.query(min_lat, max_lat, min_lon, max_lon, limit=3500)

    def inspect_point(self, lat, lon, heading=0):
        if not self.f_tiles:
            return None

        lat_c = int((lat + 90.0) / self.tile_size)
        lon_c = int((lon + 180.0) / self.tile_size)

        best_d = 1e9
        best_proj = None
        best_seg = None
        best_seg_hd = 0.0

        for dLat in (-1, 0, 1):
            for dLon in (-1, 0, 1):
                tid = ((lat_c + dLat) << 16) | ((lon_c + dLon) & 0xFFFF)
                segs = self.get_tile_segments(tid)
                if not segs:
                    continue

                for sid, slat, slon, elat, elon, hdeg, rcls, rdir, rlim, rflg in segs:
                    d, pLa, pLo, s_hd = dist_point_to_seg(lat, lon, slat, slon, elat, elon)
                    if d < best_d:
                        best_d = d
                        best_proj = (pLa, pLo)
                        best_seg_hd = s_hd
                        best_seg = {
                            "id": sid,
                            "start": [round(slat, 7), round(slon, 7)],
                            "end": [round(elat, 7), round(elon, 7)],
                            "road_class": rcls,
                            "speed_limit": rlim,
                            "direction": rdir,
                            "heading": hdeg,
                            "flags": rflg,
                            "name": self.get_road_name(sid)
                        }

        if best_seg is None:
            return None

        h_diff = 0
        if heading > 0:
            diff = abs(heading - best_seg_hd) % 360
            h_diff = 360 - diff if diff > 180 else diff

        if best_d <= 5.0: status = "optimal"
        elif best_d <= 15.0: status = "acceptable"
        else: status = "deviated"

        road_class_labels = {
            0: "Cao tốc (Motorway)",
            1: "Quốc lộ (Trunk / National Hwy)",
            2: "Tỉnh lộ (Primary / Provincial)",
            3: "Đường chính (Secondary)",
            4: "Đường phụ (Tertiary)",
            5: "Đường nhánh / Đô thị (Residential)",
            6: "Đường dịch vụ / Nội bộ (Service)"
        }

        # Calculate exact 3m roadside snap point perpendicular to road
        m_lat = 110540.0
        m_lon = 111320.0 * math.cos(math.radians(lat))
        
        # Right roadside (bearing + 90 deg)
        rad_r = math.radians(best_seg_hd + 90.0)
        snap_r_lat = best_proj[0] + (3.0 * math.cos(rad_r)) / m_lat
        snap_r_lon = best_proj[1] + (3.0 * math.sin(rad_r)) / m_lon

        # Left roadside (bearing - 90 deg)
        rad_l = math.radians(best_seg_hd - 90.0)
        snap_l_lat = best_proj[0] + (3.0 * math.cos(rad_l)) / m_lat
        snap_l_lon = best_proj[1] + (3.0 * math.sin(rad_l)) / m_lon

        return {
            "target": [lat, lon],
            "projection": [round(best_proj[0], 7), round(best_proj[1], 7)],
            "roadside_snap_right": [round(snap_r_lat, 7), round(snap_r_lon, 7)],
            "roadside_snap_left": [round(snap_l_lat, 7), round(snap_l_lon, 7)],
            "offset_meters": round(best_d, 2),
            "heading_diff": round(h_diff, 1),
            "road_bearing": round(best_seg_hd, 1),
            "status": status,
            "road_class_name": road_class_labels.get(best_seg["road_class"], f"Cấp {best_seg['road_class']}"),
            "segment": best_seg
        }

    def add_alert(self, data: dict):
        self.changes_count += 1
        la = round(float(data.get("lat", 0)), 7)
        lo = round(float(data.get("lon", 0)), 7)
        spd = int(float(data.get("speed", 0)))
        hd = int(float(data.get("heading", 0)))
        cat = data.get("category", "speed_cam")
        desc = data.get("desc", "").strip()

        if cat in CAMERA_CATEGORIES:
            self.next_cam_id += 1
            cid = self.next_cam_id
            rec = {
                "id": cid,
                "lat": la,
                "lon": lo,
                "speed": spd if cat == "speed_cam" else 0,
                "heading": hd,
                "category": cat,
                "desc": desc or ("Camera bắn tốc độ" if cat == "speed_cam" else "Camera phạt nguội / đèn đỏ"),
                "modified": True,
                "is_new": True
            }
            self.camera_records.append(rec)
            self.camera_records_map[cid] = rec
            self.camera_grid.add(rec)
            return {"record": rec, "target_type": "camera"}
        else: # SIGNS (Including Traffic Light 🚦)
            self.next_sign_id += 1
            sid = self.next_sign_id
            stype = TYPE_MAP_SIGNS.get(cat, 6 if cat == "traffic_light" else 1)
            rec = {
                "id": sid,
                "seg_id": int(data.get("seg_id", 0)),
                "lat": la,
                "lon": lo,
                "heading": hd,
                "type": stype,
                "category": cat,
                "value": spd,
                "flags": 0,
                "dist": 0,
                "desc": desc or ("Đèn giao thông" if cat == "traffic_light" else f"Biển báo ({cat})"),
                "modified": True,
                "is_new": True
            }
            self.sign_records.append(rec)
            self.sign_records_map[sid] = rec
            self.sign_grid.add(rec)
            return {"record": rec, "target_type": "sign"}

    def update_alert(self, data: dict):
        self.changes_count += 1
        item_id = data.get("id")
        item_id = int(item_id) if str(item_id).isdigit() else item_id
        new_cat = data.get("category", "speed_cam")

        la = round(float(data.get("lat", 0)), 7)
        lo = round(float(data.get("lon", 0)), 7)
        spd = int(float(data.get("speed", 0)))
        hd = int(float(data.get("heading", 0)))
        desc = data.get("desc", "").strip()

        is_dest_camera = new_cat in CAMERA_CATEGORIES

        in_camera = item_id in self.camera_records_map
        in_sign = item_id in self.sign_records_map

        if is_dest_camera:
            if in_sign:
                self.sign_grid.remove(item_id)
                del self.sign_records_map[item_id]
                self.sign_records = [s for s in self.sign_records if s["id"] != item_id]
                self.next_cam_id += 1
                item_id = self.next_cam_id

            rec = self.camera_records_map.get(item_id)
            if not rec:
                rec = {"id": item_id, "modified": True}
                self.camera_records.append(rec)
                self.camera_records_map[item_id] = rec
            else:
                self.camera_grid.remove(item_id)

            rec["lat"] = la
            rec["lon"] = lo
            rec["speed"] = spd if new_cat == "speed_cam" else 0
            rec["heading"] = hd
            rec["category"] = new_cat
            if desc: rec["desc"] = desc
            rec["modified"] = True
            self.camera_grid.add(rec)
            return {"record": rec, "target_type": "camera"}
        else: # is_dest_sign
            if in_camera:
                self.camera_grid.remove(item_id)
                del self.camera_records_map[item_id]
                self.camera_records = [c for c in self.camera_records if c["id"] != item_id]
                self.next_sign_id += 1
                item_id = self.next_sign_id

            rec = self.sign_records_map.get(item_id)
            if not rec:
                rec = {"id": item_id, "seg_id": 0, "flags": 0, "dist": 0, "modified": True}
                self.sign_records.append(rec)
                self.sign_records_map[item_id] = rec
            else:
                self.sign_grid.remove(item_id)

            stype = TYPE_MAP_SIGNS.get(new_cat, 6 if new_cat == "traffic_light" else 1)
            rec["lat"] = la
            rec["lon"] = lo
            rec["heading"] = hd
            rec["type"] = stype
            rec["category"] = new_cat
            rec["value"] = spd
            if desc: rec["desc"] = desc
            rec["modified"] = True
            self.sign_grid.add(rec)
            return {"record": rec, "target_type": "sign"}

    def delete_alert(self, item_type: str, item_id):
        self.changes_count += 1
        item_id = int(item_id) if str(item_id).isdigit() else item_id
        ok = False
        if item_id in self.camera_records_map:
            del self.camera_records_map[item_id]
            self.camera_records = [c for c in self.camera_records if c["id"] != item_id]
            self.camera_grid.remove(item_id)
            ok = True
        if item_id in self.sign_records_map:
            del self.sign_records_map[item_id]
            self.sign_records = [s for s in self.sign_records if s["id"] != item_id]
            self.sign_grid.remove(item_id)
            ok = True
        return ok


    def detect_junction(self, lat, lon, radius_m=35.0):
        if not self.f_tiles:
            return None
        m_lat = 110540.0
        m_lon = 111320.0 * math.cos(math.radians(lat))
        
        lat_c = int((lat + 90.0) / self.tile_size)
        lon_c = int((lon + 180.0) / self.tile_size)
        
        nearby_segs = []
        for dLa in (-1, 0, 1):
            for dLo in (-1, 0, 1):
                tid = ((lat_c + dLa) << 16) | ((lon_c + dLo) & 0xFFFF)
                segs = self.get_tile_segments(tid)
                if not segs:
                    continue
                for sid, slat, slon, elat, elon, hdeg, rcls, rdir, rlim, rflg in segs:
                    dx = ((slon + elon)/2.0 - lon) * m_lon
                    dy = ((slat + elat)/2.0 - lat) * m_lat
                    if math.sqrt(dx*dx + dy*dy) <= radius_m * 1.6:
                        nearby_segs.append({
                            "sid": sid, "start": (slat, slon), "end": (elat, elon),
                            "heading": hdeg, "rcls": rcls, "name": self.get_road_name(sid)
                        })
        if not nearby_segs:
            return None
            
        endpoints = []
        for s in nearby_segs:
            endpoints.append(s["start"])
            endpoints.append(s["end"])
            
        best_pt = None
        max_conn = 0
        for pt in endpoints:
            conn = sum(1 for o in endpoints if math.sqrt(((o[1]-pt[1])*m_lon)**2 + ((o[0]-pt[0])*m_lat)**2) <= 12.0)
            if conn > max_conn:
                max_conn = conn
                best_pt = pt
                
        if not best_pt or max_conn < 3:
            best_pt = (lat, lon)
            
        j_lat, j_lon = best_pt
        
        arms = []
        for s in nearby_segs:
            d_start = math.sqrt(((s["start"][1] - j_lon)*m_lon)**2 + ((s["start"][0] - j_lat)*m_lat)**2)
            d_end = math.sqrt(((s["end"][1] - j_lon)*m_lon)**2 + ((s["end"][0] - j_lat)*m_lat)**2)
            
            if d_end <= 18.0 and d_start > d_end:
                dx = (s["end"][1] - s["start"][1]) * m_lon
                dy = (s["end"][0] - s["start"][0]) * m_lat
                bearing = math.degrees(math.atan2(dx, dy)) % 360
                arms.append({"sid": s["sid"], "bearing": bearing, "name": s["name"], "rcls": s["rcls"]})
            elif d_start <= 18.0 and d_end > d_start:
                dx = (s["start"][1] - s["end"][1]) * m_lon
                dy = (s["start"][0] - s["end"][0]) * m_lat
                bearing = math.degrees(math.atan2(dx, dy)) % 360
                arms.append({"sid": s["sid"], "bearing": bearing, "name": s["name"], "rcls": s["rcls"]})
                
        # Group arms by bearing (within 35 degrees)
        clustered = []
        for a in arms:
            hd = a["bearing"]
            matched = False
            for c in clustered:
                diff = abs(c["bearing"] - hd) % 360
                diff = 360 - diff if diff > 180 else diff
                if diff <= 35:
                    matched = True
                    break
            if not matched:
                clustered.append(a)
                
        if len(clustered) < 2:
            return None
            
        results = []
        for idx, c in enumerate(clustered):
            hd = round(c["bearing"])
            # Stop line 15m back along approach vector
            app_rad = math.radians((hd + 180) % 360)
            stop_lat = j_lat + (15.0 * math.cos(app_rad)) / m_lat
            stop_lon = j_lon + (15.0 * math.sin(app_rad)) / m_lon
            
            # Post 3.5m to the right
            r_rad = math.radians(hd + 90.0)
            post_lat = stop_lat + (3.5 * math.cos(r_rad)) / m_lat
            post_lon = stop_lon + (3.5 * math.sin(r_rad)) / m_lon
            
            results.append({
                "arm_index": idx + 1,
                "name": c["name"] or f"Trục đường góc {hd}°",
                "heading": hd,
                "road_class": c["rcls"],
                "stop_line": [round(stop_lat, 7), round(stop_lon, 7)],
                "post_coords": [round(post_lat, 7), round(post_lon, 7)]
            })
            
        junction_type = "ngã 3" if len(results) == 3 else ("ngã 4" if len(results) == 4 else f"giao lộ {len(results)} nhánh")
        return {
            "center": [round(j_lat, 7), round(j_lon, 7)],
            "type_name": junction_type,
            "arms_count": len(results),
            "arms": results
        }

    def update_road_class(self, sid, new_rcls, scope="single", lat=0, lon=0):
        self.changes_count += 1
        new_rcls = max(0, min(6, int(new_rcls)))
        tiles_path = Path(self.road_files["tiles"])
        target_sids = [sid]
        
        target_name = self.get_road_name(sid)
        if scope == "named_road" and target_name and lat and lon:
            m_lat = 110540.0
            m_lon = 111320.0 * math.cos(math.radians(lat))
            lat_c = int((lat + 90.0) / self.tile_size)
            lon_c = int((lon + 180.0) / self.tile_size)
            for dLa in (-2, -1, 0, 1, 2):
                for dLo in (-2, -1, 0, 1, 2):
                    tid = ((lat_c + dLa) << 16) | ((lon_c + dLo) & 0xFFFF)
                    segs = self.get_tile_segments(tid)
                    if not segs: continue
                    for s_sid, _, _, _, _, _, _, _, _, _ in segs:
                        if self.get_road_name(s_sid) == target_name:
                            if s_sid not in target_sids:
                                target_sids.append(s_sid)
                                
        # Perform in-place write to tiles.bin with .bak backup
        ts = int(time.time())
        bak = tiles_path.with_suffix(f".bak_{ts}")
        if not bak.exists() and tiles_path.exists():
            shutil.copy2(tiles_path, bak)
            
        updated_count = 0
        if tiles_path.exists():
            with open(tiles_path, "r+b") as f:
                for sid_target in target_sids:
                    # Fast tile lookup
                    for tid, (off, sz) in self.tile_entries.items():
                        f.seek(off)
                        t_bytes = f.read(sz)
                        n_segs = sz // 28
                        found = False
                        for s_i in range(n_segs):
                            cur_sid = struct.unpack('<I', t_bytes[s_i*28 : s_i*28 + 4])[0]
                            if cur_sid == sid_target:
                                f.seek(off + s_i * 28 + 22)
                                f.write(struct.pack("<B", new_rcls))
                                updated_count += 1
                                found = True
                                break
                        if found:
                            break
                            
        self.tile_decoded_cache.clear()
        return {
            "status": "ok",
            "updated_count": updated_count,
            "target_sids_count": len(target_sids),
            "new_road_class": new_rcls,
            "road_name": target_name,
            "changes_count": self.changes_count
        }

    def batch_add_junction_alerts(self, alerts_list, save_mode="overwrite_with_backup"):
        added_records = []
        for item in alerts_list:
            res = self.add_alert(item)
            if res and "record" in res:
                added_records.append(res["record"])
                
        # Persist immediately to file!
        ts = int(time.time())
        results = {}
        if save_mode == "overwrite_with_backup":
            cam_p = Path(DEFAULT_CSV) if DEFAULT_CSV.exists() else Path(self.active_camera_file)
            sign_p = Path(DEFAULT_SPEEDMAP / "signs.bin")
            if cam_p.exists(): shutil.copy2(cam_p, cam_p.with_suffix(f".bak_{ts}"))
            if sign_p.exists(): shutil.copy2(sign_p, sign_p.with_suffix(f".bak_{ts}"))
            self.export_dataset("cameras", "csv", str(cam_p))
            self.export_dataset("signs", "bin", str(sign_p))
            results["cameras"] = str(cam_p)
            results["signs"] = str(sign_p)
            results["msg"] = f"Đã cắm và ghi thành công {len(added_records)} điểm vào file gốc:\n+ cameras.csv\n+ signs.bin"
        else:
            out_cam = DEFAULT_SPEEDMAP / f"cameras_edited_{ts}.csv"
            out_sign = DEFAULT_SPEEDMAP / f"signs_edited_{ts}.bin"
            self.export_dataset("cameras", "csv", str(out_cam))
            self.export_dataset("signs", "bin", str(out_sign))
            self.active_camera_file = str(out_cam)
            self.active_sign_file = str(out_sign)
            results["cameras"] = str(out_cam)
            results["signs"] = str(out_sign)
            results["msg"] = f"Đã cắm và tạo file mới:\n+ {out_cam.name}\n+ {out_sign.name}"
            
        self.changes_count = 0
        return {
            "status": "ok",
            "added_count": len(added_records),
            "records": added_records,
            "save_results": results
        }

    def export_dataset(self, dataset: str, target_format: str, target_path: str):
        p = Path(target_path)
        p.parent.mkdir(parents=True, exist_ok=True)

        if dataset == "cameras":
            if target_format == "csv" or p.suffix.lower() == ".csv":
                with open(p, "w", newline="", encoding="utf-8-sig") as f:
                    writer = csv.writer(f)
                    writer.writerow(["lat", "lon", "speed_limit", "heading_deg", "heading_raw", "sign_type", "type_code", "sign_name", "description"])
                    for c in self.camera_records:
                        hd_raw = (c["heading"] // 2) % 180
                        type_code = "SPEED_CAMERA" if c["speed"] > 0 else "SURVEILLANCE_CAMERA"
                        sname = "Camera phạt nguội / Bắn tốc độ" if c["speed"] > 0 else "Camera phạt nguội giám sát"
                        writer.writerow([
                            f"{c['lat']:.7f}",
                            f"{c['lon']:.7f}",
                            c["speed"],
                            c["heading"],
                            hd_raw,
                            4,
                            type_code,
                            sname,
                            c["desc"]
                        ])
            else: # BIN
                blob = bytearray()
                for c in self.camera_records:
                    lat_e7 = int(round(c["lat"] * 1e7))
                    lon_e7 = int(round(c["lon"] * 1e7))
                    blob.extend(struct.pack("<QiihH", int(c["id"]), lat_e7, lon_e7, int(c["speed"]), int(c["heading"])))
                p.write_bytes(bytes(blob))
            return len(self.camera_records)
        else: # signs
            if target_format == "csv" or p.suffix.lower() == ".csv":
                with open(p, "w", newline="", encoding="utf-8-sig") as f:
                    writer = csv.writer(f)
                    writer.writerow(["segment_id", "lat", "lon", "heading", "sign_type", "value"])
                    for s in self.sign_records:
                        writer.writerow([
                            s.get("seg_id", 0),
                            f"{s['lat']:.7f}",
                            f"{s['lon']:.7f}",
                            s["heading"],
                            s["type"],
                            s.get("value", 0)
                        ])
            else: # BIN
                records_blob = bytearray()
                for s in self.sign_records:
                    lat_e7 = int(round(s["lat"] * 1e7))
                    lon_e7 = int(round(s["lon"] * 1e7))
                    records_blob.extend(struct.pack(
                        "<IiiHBBBBH",
                        s.get("seg_id", 0), lat_e7, lon_e7,
                        int(s["heading"]), int(s["type"]), int(s.get("value", 0)),
                        s.get("flags", 0), 0, s.get("dist", 0)
                    ))
                count = len(self.sign_records)
                crc = zlib.crc32(records_blob) & 0xFFFFFFFF
                header = struct.pack("<4sHIHII", b"VNSG", 1, count, 0, count, crc)
                p.write_bytes(header + bytes(records_blob))
            return len(self.sign_records)

ENGINE = SpeedMapEngine()

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

class SpeedMapHandler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        if path == "/api/info":
            info = {
                "road_files": {k: str(v) for k, v in ENGINE.road_files.items()},
                "tiles_count": len(ENGINE.tile_entries),
                "names_count": len(ENGINE.names_cache),
                "active_camera_file": ENGINE.active_camera_file,
                "camera_filename": Path(ENGINE.active_camera_file).name,
                "cameras_count": len(ENGINE.camera_records),
                "active_sign_file": ENGINE.active_sign_file,
                "sign_filename": Path(ENGINE.active_sign_file).name,
                "signs_count": len(ENGINE.sign_records),
                "changes_count": ENGINE.changes_count,
                "center": [21.0360, 105.7830],
                "vietnam_bbox": [8.5, 102.1, 23.4, 109.5]
            }
            self.send_json(info)
            return

        elif path == "/api/roads":
            min_lat = float(query.get("min_lat", [0])[0])
            max_lat = float(query.get("max_lat", [0])[0])
            min_lon = float(query.get("min_lon", [0])[0])
            max_lon = float(query.get("max_lon", [0])[0])
            zoom = int(query.get("zoom", [15])[0])
            segs = ENGINE.query_roads(min_lat, max_lat, min_lon, max_lon, zoom)
            self.send_json({"segments": segs, "count": len(segs)})
            return

        elif path == "/api/cameras":
            min_lat = float(query.get("min_lat", [0])[0])
            max_lat = float(query.get("max_lat", [0])[0])
            min_lon = float(query.get("min_lon", [0])[0])
            max_lon = float(query.get("max_lon", [0])[0])
            cams = ENGINE.query_cameras(min_lat, max_lat, min_lon, max_lon)
            self.send_json({"cameras": cams, "count": len(cams)})
            return

        elif path == "/api/signs":
            min_lat = float(query.get("min_lat", [0])[0])
            max_lat = float(query.get("max_lat", [0])[0])
            min_lon = float(query.get("min_lon", [0])[0])
            max_lon = float(query.get("max_lon", [0])[0])
            signs = ENGINE.query_signs(min_lat, max_lat, min_lon, max_lon)
            self.send_json({"signs": signs, "count": len(signs)})
            return

        elif path == "/api/detect_junction":
            lat = float(query.get("lat", [0])[0])
            lon = float(query.get("lon", [0])[0])
            radius = float(query.get("radius", [35])[0])
            res = ENGINE.detect_junction(lat, lon, radius)
            if res:
                self.send_json(res)
            else:
                self.send_json({"error": "Không tìm thấy ngã 3/ngã 4 tại toạ độ này"}, status=404)
            return

        elif path == "/api/inspect":
            lat = float(query.get("lat", [0])[0])
            lon = float(query.get("lon", [0])[0])
            hd = float(query.get("heading", [0])[0])
            res = ENGINE.inspect_point(lat, lon, hd)
            if res:
                self.send_json(res)
            else:
                self.send_json({"error": "No road nearby"}, status=404)
            return

        elif path == "/api/export_download":
            dataset = query.get("dataset", ["cameras"])[0]
            fmt = query.get("format", ["csv"])[0]
            
            if dataset == "cameras":
                if fmt == "bin":
                    blob = bytearray()
                    for c in ENGINE.camera_records:
                        lat_e7 = int(round(c["lat"] * 1e7))
                        lon_e7 = int(round(c["lon"] * 1e7))
                        blob.extend(struct.pack("<QiihH", int(c["id"]), lat_e7, lon_e7, int(c["speed"]), int(c["heading"])))
                    data = bytes(blob)
                    mime = "application/octet-stream"
                    ext = "bin"
                else:
                    rows = [["lat", "lon", "speed_limit", "heading_deg", "heading_raw", "sign_type", "type_code", "sign_name", "description"]]
                    for c in ENGINE.camera_records:
                        hd_raw = (c["heading"] // 2) % 180
                        tc = "SPEED_CAMERA" if c["speed"] > 0 else "SURVEILLANCE_CAMERA"
                        sn = "Camera phạt nguội / Bắn tốc độ" if c["speed"] > 0 else "Camera phạt nguội giám sát"
                        rows.append([f"{c['lat']:.7f}", f"{c['lon']:.7f}", c["speed"], c["heading"], hd_raw, 4, tc, sn, c["desc"]])
                    data = "\n".join([",".join(map(str, r)) for r in rows]).encode("utf-8-sig")
                    mime = "text/csv; charset=utf-8"
                    ext = "csv"
            else: # signs
                if fmt == "csv":
                    rows = [["segment_id", "lat", "lon", "heading", "sign_type", "value"]]
                    for s in ENGINE.sign_records:
                        rows.append([s.get("seg_id", 0), f"{s['lat']:.7f}", f"{s['lon']:.7f}", s["heading"], s["type"], s.get("value", 0)])
                    data = "\n".join([",".join(map(str, r)) for r in rows]).encode("utf-8-sig")
                    mime = "text/csv; charset=utf-8"
                    ext = "csv"
                else:
                    records_blob = bytearray()
                    for s in ENGINE.sign_records:
                        lat_e7 = int(round(s["lat"] * 1e7))
                        lon_e7 = int(round(s["lon"] * 1e7))
                        records_blob.extend(struct.pack(
                            "<IiiHBBBBH",
                            s.get("seg_id", 0), lat_e7, lon_e7,
                            int(s["heading"]), int(s["type"]), int(s.get("value", 0)),
                            s.get("flags", 0), 0, s.get("dist", 0)
                        ))
                    count = len(ENGINE.sign_records)
                    crc = zlib.crc32(records_blob) & 0xFFFFFFFF
                    header = struct.pack("<4sHIHII", b"VNSG", 1, count, 0, count, crc)
                    data = header + bytes(records_blob)
                    mime = "application/octet-stream"
                    ext = "bin"

            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Disposition", f"attachment; filename={dataset}_edited_{int(time.time())}.{ext}")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        # Static assets
        if path == "/" or path == "/index.html":
            self.serve_file(BASE_DIR / "static" / "index.html", "text/html")
        elif path.startswith("/static/"):
            rel_path = path[len("/static/"):]
            file_path = BASE_DIR / "static" / rel_path
            content_type = "text/plain"
            if rel_path.endswith(".css"): content_type = "text/css"
            elif rel_path.endswith(".js"): content_type = "application/javascript"
            elif rel_path.endswith(".png"): content_type = "image/png"
            elif rel_path.endswith(".svg"): content_type = "image/svg+xml"
            self.serve_file(file_path, content_type)
        else:
            self.send_error(404, "File not found")

    def do_POST(self):
        parsed = urlparse(self.path)
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        data = json.loads(body)

        if parsed.path == "/api/update_road_class":
            sid = int(data.get("segment_id", 0))
            new_rcls = int(data.get("road_class", 3))
            scope = data.get("scope", "single")
            lat = float(data.get("lat", 0))
            lon = float(data.get("lon", 0))
            res = ENGINE.update_road_class(sid, new_rcls, scope, lat, lon)
            self.send_json(res)
            return

        elif parsed.path == "/api/batch_add_junction_alerts":
            alerts_list = data.get("alerts", [])
            save_mode = data.get("save_mode", "overwrite_with_backup")
            res = ENGINE.batch_add_junction_alerts(alerts_list, save_mode)
            self.send_json(res)
            return

        elif parsed.path == "/api/update_alert":
            res = ENGINE.update_alert(data)
            if res:
                self.send_json({
                    "status": "ok",
                    "record": res["record"],
                    "target_type": res["target_type"],
                    "changes_count": ENGINE.changes_count
                })
            else:
                self.send_json({"error": "Item not found"}, status=404)
            return

        elif parsed.path == "/api/add_alert":
            res = ENGINE.add_alert(data)
            self.send_json({
                "status": "ok",
                "record": res["record"],
                "target_type": res["target_type"],
                "changes_count": ENGINE.changes_count
            })
            return

        elif parsed.path == "/api/delete_alert":
            itype = data.get("type", "")
            iid = data.get("id")
            ok = ENGINE.delete_alert(itype, iid)
            self.send_json({"status": "ok" if ok else "error", "changes_count": ENGINE.changes_count})
            return

        elif parsed.path == "/api/save_all":
            # Smart unified persistence: writes both cameras & signs at once!
            mode = data.get("mode", "new_files") # "new_files" or "overwrite_with_backup"
            ts = int(time.time())
            results = {}

            if mode == "overwrite_with_backup":
                cam_p = Path(DEFAULT_CSV) if DEFAULT_CSV.exists() else Path(ENGINE.active_camera_file)
                sign_p = Path(DEFAULT_SPEEDMAP / "signs.bin")
                
                # Backup
                if cam_p.exists():
                    shutil.copy2(cam_p, cam_p.with_suffix(f".bak_{ts}"))
                if sign_p.exists():
                    shutil.copy2(sign_p, sign_p.with_suffix(f".bak_{ts}"))

                # Write in-place
                ENGINE.export_dataset("cameras", "csv", str(cam_p))
                ENGINE.export_dataset("signs", "bin", str(sign_p))
                ENGINE.active_camera_file = str(cam_p)
                ENGINE.active_sign_file = str(sign_p)
                results["cameras"] = str(cam_p)
                results["signs"] = str(sign_p)
                results["msg"] = f"Đã cập nhật trực tiếp vào file gốc:\n+ {cam_p}\n+ {sign_p}\n(Kèm bản sao lưu .bak an toàn)"
            else:
                # Save into new files in speedmap folder
                out_cam = DEFAULT_SPEEDMAP / f"cameras_edited_{ts}.csv"
                out_sign = DEFAULT_SPEEDMAP / f"signs_edited_{ts}.bin"
                ENGINE.export_dataset("cameras", "csv", str(out_cam))
                ENGINE.export_dataset("signs", "bin", str(out_sign))
                
                # Update active files so studio keeps using them!
                ENGINE.active_camera_file = str(out_cam)
                ENGINE.active_sign_file = str(out_sign)
                results["cameras"] = str(out_cam)
                results["signs"] = str(out_sign)
                results["msg"] = f"Đã lưu thành bộ dữ liệu mới:\n+ {out_cam.name}\n+ {out_sign.name}"

            ENGINE.changes_count = 0
            self.send_json({
                "status": "ok",
                "results": results,
                "changes_count": 0,
                "active_camera_file": ENGINE.active_camera_file,
                "active_sign_file": ENGINE.active_sign_file
            })
            return

        elif parsed.path == "/api/save_dataset":
            dataset = data.get("dataset", "cameras")
            target_fmt = data.get("format", "csv")
            target_path = data.get("path", "").strip()
            if not target_path:
                ext = ".csv" if target_fmt == "csv" else ".bin"
                target_path = str(DEFAULT_SPEEDMAP / f"{dataset}_edited_{int(time.time())}{ext}")
            
            count = ENGINE.export_dataset(dataset, target_fmt, target_path)
            self.send_json({
                "status": "ok",
                "count": count,
                "saved_path": target_path,
                "changes_count": ENGINE.changes_count
            })
            return

        elif parsed.path == "/api/set_road_files":
            tiles_path = data.get("tiles")
            idx_path = data.get("index")
            names_path = data.get("names")
            if tiles_path: ENGINE.road_files["tiles"] = Path(tiles_path)
            if idx_path: ENGINE.road_files["index"] = Path(idx_path)
            if names_path: ENGINE.road_files["names"] = Path(names_path)
            ENGINE.load_roads()
            self.send_json({"status": "ok"})
            return

        elif parsed.path == "/api/set_camera_file":
            path_str = data.get("path")
            ENGINE.load_cameras(path_str)
            self.send_json({"status": "ok", "count": len(ENGINE.camera_records), "file": ENGINE.active_camera_file})
            return

        elif parsed.path == "/api/set_sign_file":
            path_str = data.get("path")
            ENGINE.load_signs(path_str)
            self.send_json({"status": "ok", "count": len(ENGINE.sign_records), "file": ENGINE.active_sign_file})
            return

        self.send_error(404)

    def send_json(self, data, status=200):
        raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
        accept_enc = self.headers.get("Accept-Encoding", "")

        if "gzip" in accept_enc and len(raw) > 1024:
            compressed = gzip.compress(raw, compresslevel=4)
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(compressed)))
            self.end_headers()
            self.wfile.write(compressed)
        else:
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

    def serve_file(self, file_path: Path, content_type: str):
        if not file_path.exists() or not file_path.is_file():
            self.send_error(404, f"File {file_path.name} not found")
            return
        data = file_path.read_bytes()
        accept_enc = self.headers.get("Accept-Encoding", "")

        if "gzip" in accept_enc and len(data) > 1024 and (content_type.startswith("text/") or content_type == "application/javascript"):
            compressed = gzip.compress(data, compresslevel=5)
            self.send_response(200)
            self.send_header("Content-Type", f"{content_type}; charset=utf-8")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(compressed)))
            self.end_headers()
            self.wfile.write(compressed)
        else:
            self.send_response(200)
            self.send_header("Content-Type", f"{content_type}; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def log_message(self, format, *args):
        if args and len(args) > 0 and isinstance(args[0], str) and ("GET /api/roads" in args[0] or "GET /api/cameras" in args[0] or "GET /api/signs" in args[0]):
            return
        super().log_message(format, *args)

def main():
    server = ThreadedHTTPServer(("0.0.0.0", PORT), SpeedMapHandler)
    print("=" * 80)
    print(f"[*] VIETHUD SPEEDMAP STUDIO V5.3 RUNNING AT: http://localhost:{PORT}")
    print("=" * 80)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
        server.server_close()

if __name__ == "__main__":
    main()
