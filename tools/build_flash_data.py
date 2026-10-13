"""Build the on-chip data image for boards without a microSD card — from the ONE
shared data set (speedmap/, same format, same version, same signing key).

    python tools/build_flash_data.py p4          # -> data_p4/speedmap
    python tools/build_flash_data.py lite154     # -> data_lite154/speedmap
    python tools/build_flash_data.py all --publish

Card boards (3.5", 2.8", and the P4 with a card) use speedmap/ as is. A flash
profile is speedmap/'s core files + the default voice set (the optional voice
packs are left out), cut to a region around HOME when the full country does
not fit the partition (tools/map_builder/make_region_subset.py). Its
manifest.txt keeps speedmap/'s data version and is re-signed, so the device
installer treats it exactly like the full set.

--publish also copies the profile to speedmap_<profile>/ at the repo root: the
online data channel those boards update from (BOARD_DATA_PROFILE).
Flash it with:  python tools/build.py viethud_lite154 uploadfs
"""
import argparse
import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from models import ROOT  # noqa: E402

SRC = os.path.join(ROOT, 'speedmap')
CORE = ['metadata.bin', 'index.bin', 'tiles.bin', 'names.bin', 'seg_names.bin', 'cameras.bin', 'signs.bin']
MANIFEST_FILES = CORE + ['roadnames.bin']
HOME = (21.0285, 105.8542)  # Ha Noi — centre of a regional cut
# usable bytes of each board's "ffat" partition (FAT overhead + slack taken off)
PROFILES = {
    'p4': dict(budget_mb=9.4, out='data_p4', writable=False),            # partitions_p4_data.csv: 9.9 MB, read-only
    'lite154': dict(budget_mb=11.1, out='data_lite154', writable=True),  # boards/partitions_lite154.csv: 11.6 MB
}
# A flash cut covers the NORTH: every main road of the speed map (which carries
# those roads' names itself), the region's cameras and signs, plus the detailed
# road-name layer (every street incl. alleys) around HOME — the country-wide
# roadnames.bin is 14 MB.
REGION = (19.0, 23.5, 102.0, 108.5)  # Bac Bo + Bac Trung Bo (to Thanh Hoa / Nghe An)
# Free space kept on a writable flash partition so daily alert updates can be
# staged (installer needs the changed files + 0.5 MB; the Lite cut of signs.bin is ~0.9 MB).
STAGING_HEADROOM_MB = 1.8


def in_region(lat, lon):
    return REGION[0] <= lat <= REGION[1] and REGION[2] <= lon <= REGION[3]


def cut_alerts(d):
    """cameras.bin / signs.bin limited to REGION (same formats; signs header + CRC rewritten)."""
    cam = open(os.path.join(d, 'cameras.bin'), 'rb').read()
    rec = 20  # CameraPoint, packed (SpeedMapFormat.h #pragma pack(1)): u64 id, i32 lat, i32 lon, i16 limit, u16 dir
    keep = [cam[i:i + rec] for i in range(0, len(cam) - len(cam) % rec, rec)
            if in_region(*(v / 1e7 for v in struct.unpack_from('<ii', cam, i + 8)))]
    open(os.path.join(d, 'cameras.bin'), 'wb').write(b''.join(keep))
    sg = open(os.path.join(d, 'signs.bin'), 'rb').read()
    magic, ver, cnt, res, _crc, ts = struct.unpack_from('<4sHIHII', sg, 0)
    hdr = len(sg) - cnt * 20
    pts = [sg[hdr + i * 20:hdr + i * 20 + 20] for i in range(cnt)
           if in_region(*(v / 1e7 for v in struct.unpack_from('<ii', sg, hdr + i * 20 + 4)))]
    body = b''.join(pts)
    open(os.path.join(d, 'signs.bin'), 'wb').write(
        struct.pack('<4sHIHII', magic, ver, len(pts), res, zlib.crc32(body) & 0xFFFFFFFF, ts) + sg[20:hdr] + body)
    print('  region alerts: %d cameras, %d signs' % (len(keep), len(pts)))


# A flash cut keeps the roads you drive on between towns: motorway..tertiary
# (road class 1-2), REGION only, no footways. Streets and alleys are named by
# the separate road-name layer; the full OSM set (every alley) stays on the
# microSD boards.
FLASH_MAX_ROAD_CLASS = 2
SEG = 28
SEGFLAG_NO_VEHICLE = 0x20
META_FMT = '<4sH16s16sQIIfiihI64sI'


def keep_major(d, room):
    """Rewrite tiles.bin / index.bin / metadata.bin of `d` in place: REGION only,
    no footways; every class-1 road (motorway/trunk/primary) first, then class-2
    roads (secondary/tertiary) tile by tile outward from HOME while they fit in
    `room` bytes (segment 28 B + seg_names 4 B + index 28 B per tile). Segment
    ids are kept (they index seg_names.bin; make_region_subset renumbers them)."""
    idx = open(os.path.join(d, 'index.bin'), 'rb').read()
    tiles = []  # (dist, tid, bbox, class1 bytes, class2 bytes)
    with open(os.path.join(d, 'tiles.bin'), 'rb') as tf:
        for i in range(0, len(idx) - len(idx) % 28, 28):
            tid, a, b, c, e, off, size = struct.unpack_from('<IiiiiII', idx, i)
            la, lo = (a + b) / 2e7, (c + e) / 2e7
            if not in_region(la, lo):
                continue
            tf.seek(off)
            blob = tf.read(size)
            k1, k2 = bytearray(), bytearray()
            for k in range(0, len(blob) - len(blob) % SEG, SEG):
                rcls, flags = blob[k + 22], blob[k + 27]
                if flags & SEGFLAG_NO_VEHICLE:
                    continue
                if rcls == 1:
                    k1 += blob[k:k + SEG]
                elif rcls == FLASH_MAX_ROAD_CLASS:
                    k2 += blob[k:k + SEG]
            if k1 or k2:
                dist = math.hypot(la - HOME[0], (lo - HOME[1]) * math.cos(math.radians(HOME[0])))
                tiles.append([dist, tid, (a, b, c, e), k1, k2])
    cost = lambda n: n // SEG * (SEG + 4)
    used = sum(cost(len(t[3])) + 28 for t in tiles if t[3])
    radius = 0.0
    for t in sorted(tiles, key=lambda t: t[0]):
        extra = cost(len(t[4])) + (0 if t[3] else 28)
        if not t[4]:
            continue
        if used + extra > room:
            break
        used += extra
        t[3] += t[4]
        radius = t[0] * 111.0
    out_tiles, out_idx, nseg, ntile = bytearray(), bytearray(), 0, 0
    for _, tid, (a, b, c, e), keep, _k2 in sorted(tiles, key=lambda t: t[1]):
        if keep:
            out_idx += struct.pack('<IiiiiII', tid, a, b, c, e, len(out_tiles), len(keep))
            out_tiles += keep
            nseg += len(keep) // SEG
            ntile += 1
    open(os.path.join(d, 'tiles.bin'), 'wb').write(out_tiles)
    open(os.path.join(d, 'index.bin'), 'wb').write(out_idx)
    meta = list(struct.unpack_from(META_FMT, open(os.path.join(d, 'metadata.bin'), 'rb').read()))
    meta[5], meta[6] = ntile, nseg
    meta[13] = zlib.crc32(bytes(out_idx)) & 0xFFFFFFFF
    open(os.path.join(d, 'metadata.bin'), 'wb').write(struct.pack(META_FMT, *meta))
    print('  roads: class 1 in all of REGION + class 2 within ~%.0f km: %d tiles, %d segments, %.1f MB'
          % (radius, ntile, nseg, used / 1e6))


def tree_size(d):
    return sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(d) for f in fs)


def stage_source(tmp):
    """speedmap/ core files + the default voice set (no optional packs)."""
    for f in CORE:
        shutil.copy2(os.path.join(SRC, f), os.path.join(tmp, f))
    # Only sounds/vi/ — the firmware plays from /speedmap/sounds/vi/ (audio/
    # AudioPlayer.cpp VOICE_DIR); the clips loose in sounds/ duplicate it.
    # (an OSM working set from SpeedMap Studio has no clips: take the checkout's voice set)
    snd = os.path.join(SRC, 'sounds', 'vi')
    if not os.path.isdir(snd):
        snd = os.path.join(ROOT, 'speedmap', 'sounds', 'vi')
    shutil.copytree(snd, os.path.join(tmp, 'sounds', 'vi'), ignore=shutil.ignore_patterns('*.bak*'))


def write_manifest(d):
    try:
        version = open(os.path.join(SRC, 'manifest.txt'), encoding='utf-8').readline().strip()
    except OSError:  # a Studio working set has no manifest yet: the publish job writes the real one
        version = 'version ' + time.strftime('%Y.%m.%d.%H%M')
    lines = [version]
    for f in MANIFEST_FILES:
        data = open(os.path.join(d, f), 'rb').read()
        lines.append('%s %d %s' % (f, len(data), hashlib.sha256(data).hexdigest()))
    open(os.path.join(d, 'manifest.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    sig = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'sign_manifest.py'),
                          os.path.join(d, 'manifest.txt')])
    if sig.returncode != 0:
        print('  WARNING: manifest not signed (no key?) — devices will refuse an online install of it')
    return version


def build(profile, publish):
    p = PROFILES[profile]
    out = os.path.join(ROOT, p['out'], 'speedmap')
    with tempfile.TemporaryDirectory() as tmp:
        # Road names: the published Lite name channel (same file the unit would
        # download), so the image and the channel agree; built if missing.
        chan = os.path.join(ROOT, 'roadnames_lite154')
        if not os.path.exists(os.path.join(chan, 'manifest.txt')):
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'map_builder', 'publish_roadnames.py')],
                           check=True)
        names = os.path.join(chan, 'roadnames.bin')
        names_ver = open(os.path.join(chan, 'manifest.txt'), encoding='utf-8').readline().split()[1]
        budget_mb = p['budget_mb'] - os.path.getsize(names) / 1e6  # what is left for the speed map
        if p['writable']:
            budget_mb -= STAGING_HEADROOM_MB  # room to stage online data updates
        src = os.path.join(tmp, 'src')
        os.makedirs(src)
        stage_source(src)
        cut_alerts(src)
        if os.path.abspath(SRC) != os.path.join(ROOT, 'speedmap'):  # OSM road classes (the legacy set uses another scale)
            other = tree_size(src) - sum(os.path.getsize(os.path.join(src, f)) for f in ('tiles.bin', 'seg_names.bin', 'index.bin'))
            keep_major(src, budget_mb * 1e6 - other - 64 * 1024)
        if os.path.exists(out):
            shutil.rmtree(out)
        if tree_size(src) <= budget_mb * 1e6:
            shutil.copytree(src, out)
            print('[%s] whole country fits' % profile)
        else:
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'map_builder', 'make_region_subset.py'),
                            src, out, str(HOME[0]), str(HOME[1]), '%.2f' % budget_mb], check=True)
        shutil.copy2(names, os.path.join(out, 'roadnames.bin'))
        with open(os.path.join(out, 'roadnames.ver'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(names_ver + '\n')  # net/NamesUpdater: the unit knows which name release it has
    version = write_manifest(out)
    print('[%s] %s: %.2f MB, %s' % (profile, os.path.relpath(out, ROOT), tree_size(out) / 1e6, version))
    if publish:
        print('[%s] --publish is retired: the flash cut carries third-party alert data, so it is '
              'loaded over USB or pushed from this PC (tools/push_data.py --dir %s), never published.'
              % (profile, os.path.relpath(out, ROOT)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profile', choices=sorted(PROFILES) + ['all'])
    ap.add_argument('--src', help='data set to cut from (default speedmap/; SpeedMap Studio passes its working set)')
    ap.add_argument('--publish', action='store_true', help='also refresh speedmap_<profile>/ (online channel)')
    a = ap.parse_args()
    if a.src:
        global SRC
        SRC = os.path.abspath(a.src)
    for prof in (sorted(PROFILES) if a.profile == 'all' else [a.profile]):
        build(prof, a.publish)


if __name__ == '__main__':
    main()
