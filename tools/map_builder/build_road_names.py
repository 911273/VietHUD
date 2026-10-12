"""Build speedmap/roadnames.bin — the road-NAME layer of the shared data set.

Every named OSM road, alleys included (Ngõ / Ngách / Hẻm / Kiệt — the speed
map's road segments only carry the classified network, so a car in an alley
used to be labelled with the main road or nothing). Read on demand by
src/map/RoadNames.cpp on every model; same matching rules as the stand-alone
Lite's roads_hn.bin (nearest road, small penalty per road class, heading
check while moving).

    python tools/map_builder/build_road_names.py                    # whole country
    python tools/map_builder/build_road_names.py --bbox 20.6 21.5 105.3 106.3 --out x.bin
    # main roads of a region, every street (alleys too) only inside --full-bbox:
    python tools/map_builder/build_road_names.py --bbox 19 23.5 102 108.5 --major-only --full-bbox 20.6 21.5 105.3 106.3

Source: OpenStreetMap (ODbL) — data/speedmap/roads.sqlite on the PC, or the
Geofabrik vietnam-latest.osm.pbf with --pbf (the Pi's weekly job).

Layout "VHRN" v1, little endian — designed for reads straight off the card:
  header (44 B): char[4] "VHRN", u16 version=1, u16 cellE5, i32 minLatE5,
                 i32 minLonE5, u16 rows, u16 cols, u32 nameCount,
                 u32 nameIndexOff, u32 nameBlobOff, u32 cellTableOff,
                 u32 dataOff, u32 reserved
  cell table   : (rows*cols + 1) x u32 absolute offsets, prefix form — cell i's
                 data is [T[i], T[i+1]) (equal = empty), so any run of
                 neighbouring cells in a row is ONE read of the table + ONE of data
  name index   : nameCount x u32 offset into the blob
  name blob    : UTF-8 (NFC), NUL terminated
  cell data    : runs {u32 nameIdx, u8 roadClass, u8 nPts, nPts x (i16 dLatE5, i16 dLonE5)}
                 points relative to the cell's south-west corner, in 1e-5 deg
"""
import argparse
import os
import sqlite3
import struct
import unicodedata
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DB = os.path.join(ROOT, 'data', 'speedmap', 'roads.sqlite')
OUT = os.path.join(ROOT, 'roadnames', 'roadnames.bin')

CELL_E5 = 1000            # 0.01 deg ~ 1.1 km
SIMPLIFY_DEG = 0.00004    # ~4 m Douglas-Peucker tolerance
MAX_SEG_M = 300.0
HDR = struct.Struct('<4sHHiiHHIIIIII')

# Lower = more important; the firmware adds a small distance penalty per step
# so a main road wins over a parallel alley at the same distance.
ROAD_CLASS = {
    'motorway': 0, 'trunk': 1, 'primary': 2, 'secondary': 3, 'tertiary': 4,
    'unclassified': 5, 'residential': 6, 'living_street': 6,
    'motorway_link': 7, 'trunk_link': 7, 'primary_link': 7, 'secondary_link': 7, 'tertiary_link': 7,
    'service': 8,  # many named ngõ are tagged service
}
# "Main roads" for --major-only: motorway .. tertiary and their links.
MAJOR_CLASSES = {0, 1, 2, 3, 4, 7}


def simplify(coords, tol=SIMPLIFY_DEG):
    """Douglas-Peucker on (lon, lat) tuples (iterative, no shapely needed)."""
    if len(coords) < 3:
        return list(coords)
    keep = [False] * len(coords)
    keep[0] = keep[-1] = True
    stack = [(0, len(coords) - 1)]
    while stack:
        a, b = stack.pop()
        (x0, y0), (x1, y1) = coords[a], coords[b]
        dx, dy = x1 - x0, y1 - y0
        L = dx * dx + dy * dy
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            x, y = coords[i]
            if L == 0:
                d = (x - x0) ** 2 + (y - y0) ** 2
            else:
                t = ((x - x0) * dx + (y - y0) * dy) / L
                t = 0 if t < 0 else 1 if t > 1 else t
                d = (x - x0 - t * dx) ** 2 + (y - y0 - t * dy) ** 2
            if d > best:
                best, bi = d, i
        if bi >= 0 and best > tol * tol:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [c for c, k in zip(coords, keep) if k]


def roads_from_sqlite(db, bbox):
    """(name, ref, highway, [[(lon, lat), ...], ...]) from the GeoPackage-style roads.sqlite (PC)."""
    from shapely import wkb
    con = sqlite3.connect(db)
    q = 'select name, ref, highway, geometry from roads where max_lat >= ? and min_lat <= ? and max_lon >= ? and min_lon <= ?'
    for name, ref, hw, blob in con.execute(q, bbox):
        env = (blob[3] >> 1) & 7
        g = wkb.loads(bytes(blob[8 + (0, 32, 48, 48, 64)[env]:]))
        parts = list(g.geoms) if g.geom_type == 'MultiLineString' else [g]
        yield name, ref, hw, [list(p.coords) for p in parts if p.geom_type == 'LineString']


VIETNAM = (8.4, 23.5, 102.1, 109.6)
_PBF_CACHE = {}


def roads_from_pbf(pbf, bbox):
    """Same, straight from an OpenStreetMap .pbf with pyosmium (the Pi's weekly job).
    The file is parsed once per process; later calls only filter by bbox."""
    min_lat, max_lat, min_lon, max_lon = bbox
    if pbf not in _PBF_CACHE:
        _PBF_CACHE[pbf] = _parse_pbf(pbf)
    for name, ref, hw, lines in _PBF_CACHE[pbf]:
        b = bbox_of_lines(lines)
        if b[1] >= min_lat and b[0] <= max_lat and b[3] >= min_lon and b[2] <= max_lon:
            yield name, ref, hw, lines


def _parse_pbf(pbf):
    import osmium
    import tempfile
    min_lat, max_lat, min_lon, max_lon = VIETNAM
    out = []

    class H(osmium.SimpleHandler):
        def way(self, w):
            hw = w.tags.get('highway')
            if hw not in ROAD_CLASS or not (w.tags.get('name') or w.tags.get('ref')):
                return
            pts = [(n.lon, n.lat) for n in w.nodes if n.location.valid()]
            if len(pts) < 2:
                return
            if max(p[1] for p in pts) < min_lat or min(p[1] for p in pts) > max_lat or                max(p[0] for p in pts) < min_lon or min(p[0] for p in pts) > max_lon:
                return
            out.append((w.tags.get('name'), w.tags.get('ref'), hw, [pts]))

    with tempfile.TemporaryDirectory() as tmp:  # node locations on disk, not RAM
        H().apply_file(pbf, locations=True, idx='sparse_file_array,' + os.path.join(tmp, 'nodes.idx'))
    return out


def bbox_of_lines(lines):
    xs = [x for ln in lines for x, _ in ln]
    ys = [y for ln in lines for _, y in ln]
    return min(ys), max(ys), min(xs), max(xs)


def clean_name(name, ref):
    name = ' '.join((name or '').split())
    ref = ' '.join((ref or '').split())
    if not name and ref:
        name = ref.split(';')[0]
        if name[:2] in ('QL', 'TL', 'DT', 'ĐT', 'CT') and name[2:3] not in ('.', ' '):
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


def build(db, out_path, bbox, major_only=False, full_bbox=None, pbf=None):
    bbox = tuple(bbox) if bbox else VIETNAM
    min_lat, max_lat, min_lon, max_lon = bbox
    min_lat_e5 = int(min_lat * 1e5) // CELL_E5 * CELL_E5
    min_lon_e5 = int(min_lon * 1e5) // CELL_E5 * CELL_E5
    rows = (round(max_lat * 1e5) - min_lat_e5) // CELL_E5 + 1
    cols = (round(max_lon * 1e5) - min_lon_e5) // CELL_E5 + 1

    names, name_idx = [], {}
    cells = defaultdict(list)  # cellId -> [(nameIdx, cls, [(latE5, lonE5), ...])]
    ways = 0
    src = roads_from_pbf(pbf, bbox) if pbf else roads_from_sqlite(db, bbox)
    for name, ref, hw, lines in src:
        cls = ROAD_CLASS.get(hw)
        if cls is None or not lines:
            continue
        if major_only and cls not in MAJOR_CLASSES:
            if full_bbox is None:
                continue
            g = bbox_of_lines(lines)
            if g[1] < full_bbox[0] or g[0] > full_bbox[1] or g[3] < full_bbox[2] or g[2] > full_bbox[3]:
                continue
        label = clean_name(name, ref)
        if not label:
            continue
        if label not in name_idx:
            name_idx[label] = len(names)
            names.append(label)
        ni = name_idx[label]
        ways += 1
        for line in lines:
            if len(line) < 2:
                continue
            coords = densify(simplify(line))
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

    name_blob = bytearray()
    name_offs = []
    for n in names:
        name_offs.append(len(name_blob))
        name_blob += n.encode('utf-8') + b'\0'

    ncell = rows * cols
    name_index_off = HDR.size
    name_blob_off = name_index_off + 4 * len(names)
    cell_table_off = name_blob_off + len(name_blob)
    cell_table_off += (-cell_table_off) % 4
    data_off = cell_table_off + 4 * (ncell + 1)

    data = bytearray()
    table = bytearray()
    pts = 0
    for cid in range(ncell):
        table += struct.pack('<I', data_off + len(data))
        if cid not in cells:
            continue
        r, c = divmod(cid, cols)
        base_lat, base_lon = min_lat_e5 + r * CELL_E5, min_lon_e5 + c * CELL_E5
        for ni, cls, run in cells[cid]:
            data += struct.pack('<IBB', ni, cls, len(run))
            for la, lo in run:
                data += struct.pack('<hh', la - base_lat, lo - base_lon)
                pts += 1
    table += struct.pack('<I', data_off + len(data))

    out = bytearray(HDR.pack(b'VHRN', 1, CELL_E5, min_lat_e5, min_lon_e5, rows, cols, len(names),
                             name_index_off, name_blob_off, cell_table_off, data_off, 0))
    out += b''.join(struct.pack('<I', o) for o in name_offs)
    out += name_blob
    out += b'\0' * (cell_table_off - len(out))
    out += table
    out += data
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(out)
    print(f'{ways} named ways, {len(names)} names, {len(cells)} cells used of {ncell}, {pts} points '
          f'-> {out_path} ({len(out) / 1e6:.2f} MB)')
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--db', default=DB, help='roads.sqlite (PC)')
    ap.add_argument('--pbf', help='OpenStreetMap .pbf instead of --db (needs pyosmium)')
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--bbox', nargs=4, type=float, metavar=('MINLAT', 'MAXLAT', 'MINLON', 'MAXLON'))
    ap.add_argument('--major-only', action='store_true', help='only main roads (motorway..tertiary + links) ...')
    ap.add_argument('--full-bbox', nargs=4, type=float, metavar=('MINLAT', 'MAXLAT', 'MINLON', 'MAXLON'),
                    help='... except inside this box, where every named road is kept')
    a = ap.parse_args()
    build(a.db, a.out, a.bbox, a.major_only, a.full_bbox, a.pbf)


if __name__ == '__main__':
    main()
