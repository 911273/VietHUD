"""SpeedMap Studio data layer: alert types, the edits layer, and the two jobs
(build from OpenStreetMap, publish to the Pi).

Data flow (2026-10-13):
  OSM vietnam-latest.osm.pbf  --build-->  data/studio/speedmap/   (working set, every way on the ground)
  Studio edits  --saved at once-->  data/custom_alerts/{studio.csv,suppress.csv}  (git-tracked, re-applied
                                    at every rebuild: added/changed points, OSM points you deleted)
  Publish: data/studio/speedmap  --signed-->  Pi /var/www/viethud/speedmap/          (3.5", 2.8", P4)
           its Lite cut            --signed-->  Pi /var/www/viethud/speedmap_lite154/  (Lite 1.54")
           + local flash images for the Lite/P4 (tools/build_flash_data.py), edits committed to git.
Data is never put on GitHub (size), firmware is (tools/release.py).
"""
import csv
import datetime
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORK = REPO / 'data' / 'studio' / 'speedmap'
FLASH = REPO / 'data' / 'studio' / 'flash' / 'speedmap'  # coarse main-road set the flash cuts (Lite, P4 w/o card) come from
FLASH_BBOX = ('19.0', '23.5', '102.0', '108.5')  # = build_flash_data.REGION
EDITS = REPO / 'data' / 'custom_alerts'
PBF = REPO / 'data' / 'osm' / 'vietnam-latest.osm.pbf'
PBF_URL = 'https://download.geofabrik.de/asia/vietnam-latest.osm.pbf'
PI_HOST = 'admin@100.107.34.92'
PI_WEB = '/var/www/viethud'
CORE = ['metadata.bin', 'index.bin', 'tiles.bin', 'names.bin', 'seg_names.bin', 'cameras.bin', 'signs.bin']
PY = sys.executable

# Studio category <-> firmware TrafficSignType (+ subType: 0 start, 1 end).
CATS = {
    'speed_cam': (4, 0), 'red_light_cam': (4, 0),
    'sign_speed': (1, 0), 'sign_r420': (2, 0), 'sign_r421': (2, 1),
    'sign_overtake': (3, 0), 'sign_overtake_end': (3, 1),
    'traffic_light': (6, 0), 'toll': (5, 0), 'danger': (10, 0),
}
CAT_LABEL = {
    'speed_cam': 'Camera bắn tốc độ', 'red_light_cam': 'Camera phạt nguội', 'sign_speed': 'Biển giới hạn tốc độ',
    'sign_r420': 'Bắt đầu khu dân cư', 'sign_r421': 'Hết khu dân cư', 'sign_overtake': 'Cấm vượt',
    'sign_overtake_end': 'Hết cấm vượt', 'traffic_light': 'Đèn tín hiệu', 'toll': 'Trạm thu phí',
    'danger': 'Nguy hiểm',
}


def category_of(sign_type, sub_type=0):
    for c, (t, s) in CATS.items():
        if c in ('speed_cam', 'red_light_cam'):
            continue
        if t == sign_type and s == sub_type:
            return c
    return {1: 'sign_speed', 2: 'sign_r420', 3: 'sign_overtake', 5: 'toll', 6: 'traffic_light'}.get(sign_type, 'danger')


def dataset_dir():
    return WORK if (WORK / 'metadata.bin').exists() else REPO / 'speedmap'


# ------------------------------------------------------------------ edits layer
class Edits:
    """Your own points (added / changed in Studio) and the OSM points you deleted.
    Rows are rewritten on every change — small files, git-tracked."""
    FIELDS = ['eid', 'lat', 'lon', 'sign_type', 'sub_type', 'speed_limit', 'heading', 'category', 'desc']

    def __init__(self):
        EDITS.mkdir(parents=True, exist_ok=True)
        self.points = {}   # eid -> row dict
        self.suppress = []  # (lat, lon, sign_type)
        self.load()

    def load(self):
        p = EDITS / 'studio.csv'
        if p.exists():
            with open(p, encoding='utf-8-sig') as f:
                for r in csv.DictReader(f):
                    self.points[r['eid']] = r
        p = EDITS / 'suppress.csv'
        if p.exists():
            with open(p, encoding='utf-8-sig') as f:
                self.suppress = [(float(r['lat']), float(r['lon']), int(r['sign_type'])) for r in csv.DictReader(f)]

    def save(self):
        with open(EDITS / 'studio.csv', 'w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, fieldnames=self.FIELDS)
            w.writeheader()
            for r in self.points.values():
                w.writerow({k: r.get(k, '') for k in self.FIELDS})
        with open(EDITS / 'suppress.csv', 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['lat', 'lon', 'sign_type'])
            for la, lo, st in self.suppress:
                w.writerow(['%.7f' % la, '%.7f' % lo, st])

    def count(self):
        return len(self.points) + len(self.suppress)

    def upsert(self, eid, rec):
        t, sub = CATS.get(rec['category'], (rec.get('type', 10), rec.get('sub', 0)))
        self.points[str(eid)] = dict(eid=str(eid), lat='%.7f' % rec['lat'], lon='%.7f' % rec['lon'], sign_type=t,
                                     sub_type=sub, speed_limit=int(rec.get('speed', rec.get('value', 0)) or 0),
                                     heading=int(rec.get('heading', 0) or 0), category=rec['category'],
                                     desc=rec.get('desc', ''))
        self.save()

    def remove_or_suppress(self, eid, rec):
        if str(eid) in self.points:
            del self.points[str(eid)]
        else:  # an OSM point: keep it out of every future build
            t, _ = CATS.get(rec.get('category'), (rec.get('type', 10), 0))
            self.suppress.append((rec['lat'], rec['lon'], t))
        self.save()

    def match(self, lat, lon, sign_type):
        """eid of one of our points at this spot (records come back from a rebuild with new ids)."""
        for eid, r in self.points.items():
            if int(r['sign_type']) == sign_type and abs(float(r['lat']) - lat) < 2e-5 and abs(float(r['lon']) - lon) < 2e-5:
                return eid
        return None


# ------------------------------------------------------------------ jobs
class Job:
    def __init__(self):
        self.name, self.running, self.ok, self.lines, self.started = None, False, None, [], 0
        self.lock = threading.Lock()

    def log(self, msg):
        with self.lock:
            self.lines.append('%s %s' % (time.strftime('%H:%M:%S'), msg))
            self.lines = self.lines[-400:]
        print('[job]', msg, flush=True)

    def start(self, name, fn, *args):
        if self.running:
            return False
        self.name, self.running, self.ok, self.lines, self.started = name, True, None, [], time.time()

        def run():
            try:
                fn(self, *args)
                self.ok = True
                self.log('XONG.')
            except Exception as e:
                self.ok = False
                self.log('LỖI: %s' % e)
            finally:
                self.running = False
        threading.Thread(target=run, daemon=True).start()
        return True

    def state(self):
        with self.lock:
            return dict(name=self.name, running=self.running, ok=self.ok, lines=list(self.lines),
                        elapsed=int(time.time() - self.started) if self.started else 0)

    def run(self, cmd, cwd=None):
        self.log('> ' + ' '.join(str(c) for c in cmd))
        p = subprocess.Popen([str(c) for c in cmd], cwd=cwd or REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding='utf-8', errors='replace')
        for line in p.stdout:
            line = line.rstrip()
            if line:
                self.log(line[-300:])
        if p.wait() != 0:
            raise RuntimeError('lệnh lỗi: %s' % ' '.join(str(c) for c in cmd[:3]))


def download_pbf(job):
    PBF.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(PBF_URL)
    if PBF.exists():
        req.add_header('If-Modified-Since', time.strftime('%a, %d %b %Y %H:%M:%S GMT', time.gmtime(PBF.stat().st_mtime)))
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            total = int(r.headers.get('Content-Length', 0))
            tmp = PBF.with_suffix('.part')
            got, last = 0, 0
            with open(tmp, 'wb') as f:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    got += len(b)
                    if got - last > 25 << 20:
                        last = got
                        job.log('tải OSM %d/%d MB' % (got >> 20, total >> 20))
            os.replace(tmp, PBF)
            job.log('đã tải bản OSM mới (%d MB)' % (got >> 20))
    except urllib.error.HTTPError as e:
        if e.code != 304:
            raise
        job.log('bản OSM trên máy đã là mới nhất (%s)' % time.strftime('%Y-%m-%d', time.localtime(PBF.stat().st_mtime)))


def job_osm_build(job, roads='everything', reload_cb=None):
    download_pbf(job)
    out = WORK.parent / 'speedmap.new'
    if out.exists():
        shutil.rmtree(out)
    idx = WORK.parent / 'nodes.idx'
    job.log('dựng dữ liệu từ OSM (%s) — mất khoảng 15–30 phút ...' % roads)
    job.run([PY, REPO / 'tools' / 'viethud-pipeline' / 'synthesize_and_pack.py', '--pbf', PBF, '--out', out,
             '--roads', roads, '--simplify-m', '4', '--custom-alerts', EDITS,
             '--osm-index', 'sparse_file_array,%s' % idx], cwd=REPO / 'tools' / 'viethud-pipeline')
    if idx.exists():
        idx.unlink()
    if WORK.exists():
        shutil.rmtree(WORK)
    os.replace(out, WORK)
    job.log('bộ dữ liệu làm việc: %s' % WORK)
    if reload_cb:
        reload_cb()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def write_manifest(d, version, files):
    lines = ['version %s' % version] + ['%s %d %s' % (f, os.path.getsize(d / f), sha256(d / f)) for f in files]
    (d / 'manifest.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    subprocess.run([PY, REPO / 'tools' / 'sign_manifest.py', d / 'manifest.txt'], check=True)


def pi_publish(job, local_dir, channel, files):
    """Upload to <web>/<channel>.new, then swap in one step so a unit never
    reads a half-updated channel."""
    tmp = '%s/%s.new' % (PI_WEB, channel)
    job.run(['ssh', '-o', 'BatchMode=yes', PI_HOST, 'rm -rf %s && mkdir -p %s' % (tmp, tmp)])
    for f in files + ['manifest.txt', 'manifest.txt.sig']:
        job.run(['scp', '-q', '-o', 'BatchMode=yes', local_dir / f, '%s:%s/%s' % (PI_HOST, tmp, f)])
    job.run(['ssh', '-o', 'BatchMode=yes', PI_HOST,
             'cd %s && rm -rf %s.old && ([ -d %s ] && mv %s %s.old || true) && mv %s.new %s && rm -rf %s.old'
             % (PI_WEB, channel, channel, channel, channel, channel, channel, channel)])
    job.log('Pi: %s/%s/ cập nhật' % (PI_WEB, channel))


def job_publish(job, notes=''):
    if not (WORK / 'metadata.bin').exists():
        raise RuntimeError('chưa có bộ dữ liệu OSM — bấm "Cập nhật OSM" trước')
    version = (datetime.datetime.utcnow() + datetime.timedelta(hours=7)).strftime('%Y.%m.%d.%H%M')
    job.log('phát hành dữ liệu %s' % version)
    pub = WORK.parent / 'publish' / 'speedmap'
    if pub.parent.exists():
        shutil.rmtree(pub.parent)
    pub.mkdir(parents=True)
    for f in CORE:
        shutil.copy2(WORK / f, pub / f)
    write_manifest(pub, version, CORE)
    pi_publish(job, pub, 'speedmap', CORE)

    # Lite: a cut of the same OSM release that fits its 11.6 MB FFat — main roads of
    # the north, simplified at 15 m (the 4 m full set is ~30x too big) — + local flash images.
    job.log('dựng bản đường chính miền Bắc cho flash (Lite / P4 không thẻ) ...')
    if FLASH.exists():
        shutil.rmtree(FLASH)
    job.run([PY, REPO / 'tools' / 'viethud-pipeline' / 'synthesize_and_pack.py', '--pbf', PBF, '--out', FLASH,
             '--roads', 'major', '--bbox', *FLASH_BBOX, '--simplify-m', '15', '--custom-alerts', EDITS],
            cwd=REPO / 'tools' / 'viethud-pipeline')
    job.run([PY, REPO / 'tools' / 'build_flash_data.py', 'all', '--src', FLASH])
    cut = REPO / 'data_lite154' / 'speedmap'
    lite_files = [f for f in CORE if (cut / f).exists()]
    write_manifest(cut, version, lite_files)
    pi_publish(job, cut, 'speedmap_lite154', lite_files)

    # The edits layer is yours: keep it in git.
    if any(EDITS.glob('*.csv')):
        job.run(['git', 'add', '--', 'data/custom_alerts'])
    if subprocess.run(['git', 'diff', '--cached', '--quiet', '--', 'data/custom_alerts'], cwd=REPO).returncode:
        job.run(['git', 'commit', '-m', 'data: Studio edits for release %s\n\n%s' % (version, notes),
                 '--', 'data/custom_alerts'])
        job.run(['git', 'pull', '--rebase', '--autostash', 'origin', 'main'])
        job.run(['git', 'push', 'origin', 'HEAD:main'])
    job.log('dữ liệu %s đã lên Pi (máy sẽ nhận ở lần kiểm tra tuần, hoặc bấm kiểm tra cập nhật)' % version)
