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
import os
import shutil
import struct
import subprocess
import sys
import tempfile
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


def tree_size(d):
    return sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(d) for f in fs)


def stage_source(tmp):
    """speedmap/ core files + the default voice set (no optional packs)."""
    for f in CORE:
        shutil.copy2(os.path.join(SRC, f), os.path.join(tmp, f))
    # Only sounds/vi/ — the firmware plays from /speedmap/sounds/vi/ (audio/
    # AudioPlayer.cpp VOICE_DIR); the clips loose in sounds/ duplicate it.
    shutil.copytree(os.path.join(SRC, 'sounds', 'vi'), os.path.join(tmp, 'sounds', 'vi'),
                    ignore=shutil.ignore_patterns('*.bak*'))


def write_manifest(d):
    version = open(os.path.join(SRC, 'manifest.txt'), encoding='utf-8').readline().strip()
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
    ap.add_argument('--publish', action='store_true', help='also refresh speedmap_<profile>/ (online channel)')
    a = ap.parse_args()
    for prof in (sorted(PROFILES) if a.profile == 'all' else [a.profile]):
        build(prof, a.publish)


if __name__ == '__main__':
    main()
