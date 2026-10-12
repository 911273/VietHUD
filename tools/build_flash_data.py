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
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from models import ROOT  # noqa: E402

SRC = os.path.join(ROOT, 'speedmap')
CORE = ['metadata.bin', 'index.bin', 'tiles.bin', 'names.bin', 'seg_names.bin', 'cameras.bin', 'signs.bin']
HOME = (21.0285, 105.8542)  # Ha Noi — centre of a regional cut
# usable bytes of each board's "ffat" partition (FAT overhead + slack taken off)
PROFILES = {
    'p4': dict(budget_mb=12.8, out='data_p4'),       # partitions_p4_data.csv: 13.5 MB
    'lite154': dict(budget_mb=9.4, out='data_lite154'),  # boards/partitions_lite154.csv: 9.9 MB
}


def tree_size(d):
    return sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(d) for f in fs)


def stage_source(tmp):
    """speedmap/ core files + the default voice set (no optional packs)."""
    for f in CORE:
        shutil.copy2(os.path.join(SRC, f), os.path.join(tmp, f))
    shutil.copytree(os.path.join(SRC, 'sounds'), os.path.join(tmp, 'sounds'),
                    ignore=shutil.ignore_patterns('packs', '*.bak*'))


def write_manifest(d):
    version = open(os.path.join(SRC, 'manifest.txt'), encoding='utf-8').readline().strip()
    lines = [version]
    for f in CORE:
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
    budget = p['budget_mb'] * 1e6
    with tempfile.TemporaryDirectory() as tmp:
        stage_source(tmp)
        if os.path.exists(out):
            shutil.rmtree(out)
        if tree_size(tmp) <= budget:
            shutil.copytree(tmp, out)
            print('[%s] whole country fits' % profile)
        else:
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'map_builder', 'make_region_subset.py'),
                            tmp, out, str(HOME[0]), str(HOME[1]), str(p['budget_mb'])], check=True)
    version = write_manifest(out)
    print('[%s] %s: %.2f MB, %s' % (profile, os.path.relpath(out, ROOT), tree_size(out) / 1e6, version))
    if publish:
        pub = os.path.join(ROOT, 'speedmap_' + profile)
        if os.path.exists(pub):
            shutil.rmtree(pub)
        os.makedirs(pub)
        # Only what the online updater downloads (manifest-listed files);
        # voices ship in the flash image and in speedmap/sounds.
        for f in CORE + ['manifest.txt', 'manifest.txt.sig']:
            shutil.copy2(os.path.join(out, f), os.path.join(pub, f))
        print('[%s] published to %s' % (profile, os.path.relpath(pub, ROOT)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profile', choices=sorted(PROFILES) + ['all'])
    ap.add_argument('--publish', action='store_true', help='also refresh speedmap_<profile>/ (online channel)')
    a = ap.parse_args()
    for prof in (sorted(PROFILES) if a.profile == 'all' else [a.profile]):
        build(prof, a.publish)


if __name__ == '__main__':
    main()
