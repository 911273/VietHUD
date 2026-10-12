"""Build data/roads_hn.bin (road-name lookup for live GPS) from the OSM roads.sqlite.

The file is embedded in the firmware (board_build.embed_files) and read by RoadNameManager.

Layout, little endian:
  header (40 B):
    char[4] "VHRD", u16 version=1, u16 cellE5, i32 minLatE5, i32 minLonE5,
    u16 rows, u16 cols, u32 cellCount, u32 nameCount,
    u32 cellIndexOff, u32 nameIndexOff, u32 nameBlobOff
  cell index : cellCount x {u32 cellId (row*cols+col), u32 dataOff}   sorted by cellId
  name index : nameCount x u32 offset into the name blob
  name blob  : UTF-8, NUL terminated (NFC, so the firmware font finds every glyph)
  cell data  : u16 runCount, then runCount x
                 {u16 nameIdx, u8 roadClass, u8 nPts, nPts x (i16 dLatE5, i16 dLonE5)}
               points relative to the cell's south-west corner, in 1e-5 deg (~1.1 m).
A run is a piece of polyline whose segments start inside the cell. Segments are cut to
<= MAX_SEG_M so the firmware only has to look at the 3x3 cells around the car.

Usage: python tools/build_road_db.py [path/to/roads.sqlite]
"""
import os
import sqlite3
import struct
import sys
import unicodedata
from collections import defaultdict

from shapely import wkb
from shapely.geometry import LineString

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\phamq\radar_car\data\speedmap\roads.sqlite'
OUT = os.path.join(ROOT, 'data', 'roads_hn.bin')

MIN_LAT, MAX_LAT, MIN_LON, MAX_LON = 19.2, 22.8, 103.9, 107.8
CELL_E5 = 1000            # 0.01 deg ~ 1.1 km
SIMPLIFY_DEG = 0.00004    # ~4 m Douglas-Peucker tolerance
MAX_SEG_M = 300.0

# Lower = more important. The firmware adds a small distance penalty per class step so a
# main road wins over a parallel alley at the same distance.
ROAD_CLASS = {
    'motorway': 0, 'trunk': 1, 'primary': 2, 'secondary': 3, 'tertiary': 4,
    'unclassified': 5, 'residential': 6, 'living_street': 6,
    'motorway_link': 7, 'trunk_link': 7, 'primary_link': 7, 'secondary_link': 7, 'tertiary_link': 7,
}


def gpkg_geom(blob):
    flags = blob[3]
    env = (flags >> 1) & 7
    return wkb.loads(bytes(blob[8 + (0, 32, 48, 48, 64)[env]:]))


def clean_name(name, ref):
    name = ' '.join((name or '').split())
    ref = ' '.join((ref or '').split())
    if not name and ref:
        name = ref.split(';')[0]
        if name[:2] in ('QL', 'TL', 'DT', 'ĐT', 'CT') and not name[2:3] in ('.', ' '):
            name = name[:2] + '.' + name[2:]
    return unicodedata.normalize('NFC', name)


def densify(coords):
    out = [coords[0]]
    for (x0, y0), (x1, y1) in zip(coords, coords[1:]):
        dist = ((x1 - x0) * 103600) ** 2 + ((y1 - y0) * 111139) ** 2
        n = int(dist ** 0.5 // MAX_SEG_M) + 1
        for k in range(1, n + 1):
            out.append((x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n))
    return out


def main():
    min_lat_e5, min_lon_e5 = round(MIN_LAT * 1e5), round(MIN_LON * 1e5)
    rows = (round(MAX_LAT * 1e5) - min_lat_e5) // CELL_E5 + 1
    cols = (round(MAX_LON * 1e5) - min_lon_e5) // CELL_E5 + 1

    con = sqlite3.connect(DB)
    names, name_idx = [], {}
    cells = defaultdict(list)   # cellId -> [(nameIdx, cls, [(latE5, lonE5), ...])]
    ways = pts_total = 0
    q = ("select name, ref, highway, geometry from roads where max_lat >= ? and min_lat <= ? "
         "and max_lon >= ? and min_lon <= ?")
    for name, ref, hw, blob in con.execute(q, (MIN_LAT, MAX_LAT, MIN_LON, MAX_LON)):
        cls = ROAD_CLASS.get(hw)
        if cls is None:
            continue
        label = clean_name(name, ref)
        if not label:
            continue
        geom = gpkg_geom(blob)
        lines = list(geom.geoms) if geom.geom_type == 'MultiLineString' else [geom]
        if label not in name_idx:
            name_idx[label] = len(names)
            names.append(label)
        ni = name_idx[label]
        ways += 1
        for line in lines:
            if not isinstance(line, LineString) or len(line.coords) < 2:
                continue
            coords = densify(list(line.simplify(SIMPLIFY_DEG, preserve_topology=False).coords))
            e5 = [(round(y * 1e5), round(x * 1e5)) for x, y in coords]
            run, run_cell = [], None
            for a, b in zip(e5, e5[1:]):
                r, c = (a[0] - min_lat_e5) // CELL_E5, (a[1] - min_lon_e5) // CELL_E5
                if not (0 <= r < rows and 0 <= c < cols):
                    if run:
                        cells[run_cell].append((ni, cls, run))
                    run, run_cell = [], None
                    continue
                cell = r * cols + c
                if cell != run_cell or len(run) >= 255:
                    if run:
                        cells[run_cell].append((ni, cls, run))
                    run, run_cell = [a], cell
                run.append(b)
            if run:
                cells[run_cell].append((ni, cls, run))

    # Serialise
    name_blob = bytearray()
    name_offs = []
    for n in names:
        name_offs.append(len(name_blob))
        name_blob += n.encode('utf-8') + b'\0'

    cell_ids = sorted(cells)
    hdr_size = 40
    cell_index_off = hdr_size
    name_index_off = cell_index_off + 8 * len(cell_ids)
    name_blob_off = name_index_off + 4 * len(names)
    data_off = name_blob_off + len(name_blob)
    data_off += (-data_off) % 4

    cell_data = bytearray()
    cell_index = bytearray()
    for cid in cell_ids:
        r, c = divmod(cid, cols)
        base_lat, base_lon = min_lat_e5 + r * CELL_E5, min_lon_e5 + c * CELL_E5
        cell_index += struct.pack('<II', cid, data_off + len(cell_data))
        runs = cells[cid]
        cell_data += struct.pack('<H', len(runs))
        for ni, cls, run in runs:
            cell_data += struct.pack('<HBB', ni, cls, len(run))
            for la, lo in run:
                cell_data += struct.pack('<hh', la - base_lat, lo - base_lon)
                pts_total += 1
        cell_data += b'\0' * ((-len(cell_data)) % 2)

    out = bytearray(struct.pack('<4sHHiiHHIIIII', b'VHRD', 1, CELL_E5, min_lat_e5, min_lon_e5, rows, cols,
                                len(cell_ids), len(names), cell_index_off, name_index_off, name_blob_off))
    assert len(out) == hdr_size
    out += cell_index
    out += b''.join(struct.pack('<I', o) for o in name_offs)
    out += name_blob
    out += b'\0' * (data_off - len(out))
    out += cell_data
    with open(OUT, 'wb') as f:
        f.write(out)
    with open(os.path.join(ROOT, 'data', 'road_names_charset.txt'), 'w', encoding='utf-8') as f:
        f.write(''.join(sorted(set(''.join(names)))))
    print(f'{ways} named ways, {len(names)} names, {len(cell_ids)} cells, {pts_total} points '
          f'-> {OUT} ({len(out) / 1024 / 1024:.2f} MB)')


if __name__ == '__main__':
    main()
