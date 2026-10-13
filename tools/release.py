"""Release VietHUD firmware for EVERY model at once — one version, one manifest.

    python tools/release.py 4.0.1 --notes "Sửa lỗi GPS"            # build + stage
    python tools/release.py 4.0.1 --notes "..." --upload-pi         # + copy to the Pi
    python tools/release.py 4.0.1 --skip-build                      # stage existing builds

What it does:
  1. sets VIETHUD_FW_VERSION in src/core/Version.h (the one platform version);
  2. builds every model in tools/models.py (S3 + P4 toolchains, tools/build.py);
  3. checks each image: right chip id in the header, its own hardware marker
     (VIETHUD-HW-ID:<model>; — net/FwUpdater.cpp), the version string;
  4. stages firmware/<version>/<model>.bin and writes firmware/release.json —
     the single manifest every device reads and picks its own image from;
  5. also rewrites the pre-4.0 per-model firmware/<model>/version.json so units
     still on 3.x find the release too (they only know that file);
  6. with --upload-pi, copies the release to the Pi web root.
Committing / pushing firmware/ to GitHub stays a manual, reviewed step (it is
what devices away from home download from).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from models import MODELS, ROOT  # noqa: E402
from build import image_path, run_env  # noqa: E402

CHIP_IDS = {'esp32s3': 9, 'esp32p4': 18}  # esp_image_header_t.chip_id
BOOTLOADER_ADDR = {'esp32s3': 0x0, 'esp32p4': 0x2000}  # where a USB flash writes bootloader.bin
BOOT_APP0 = os.path.join(os.path.expanduser('~'), '.platformio', 'packages', 'framework-arduinoespressif32',
                         'tools', 'partitions', 'boot_app0.bin')
PI_HOST = 'admin@100.107.34.92'
PI_WEBROOT = '/var/www/viethud'
URL_BASES = {  # per-route keys of the legacy version.json
    'url_pi4': 'http://192.168.1.65/viethud/',
    'url_pi4_mdns': 'http://homebridge.local/viethud/',
    'url_pi4_tailscale': 'http://100.107.34.92/viethud/',
    'url_github': 'https://raw.githubusercontent.com/911273/VietHUD/main/',
}


def set_version(ver):
    p = os.path.join(ROOT, 'src', 'core', 'Version.h')
    s = open(p, encoding='utf-8').read()
    s2 = re.sub(r'(#define VIETHUD_FW_VERSION ")[0-9.]+(")', r'\g<1>%s\g<2>' % ver, s)
    if s2 != s:
        open(p, 'w', encoding='utf-8', newline='\n').write(s2)
        print('src/core/Version.h -> %s' % ver)


def check_image(model, m, data, ver):
    if data[0] != 0xE9:
        sys.exit('%s: not an ESP image' % model)
    chip = data[12] | (data[13] << 8)
    if chip != CHIP_IDS[m['chip']]:
        sys.exit('%s: image chip id %d, expected %s (%d)' % (model, chip, m['chip'], CHIP_IDS[m['chip']]))
    markers = set(re.findall(rb'VIETHUD-HW-ID:([a-z0-9]+);', data))
    if markers != {model.encode()}:
        sys.exit('%s: hardware marker(s) %s, expected exactly %s' % (model, markers or '{}', model))
    if ver.encode() not in data:
        sys.exit('%s: built image does not contain version %s' % (model, ver))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('version', help='x.y.z, newer than every deployed unit')
    ap.add_argument('--notes', default='', help='release notes shown in the update prompt (<=120 chars)')
    ap.add_argument('--skip-build', action='store_true', help='stage the images already in .pio/build')
    ap.add_argument('--upload-pi', action='store_true', help='also copy the release to the Pi web root')
    a = ap.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+', a.version):
        sys.exit('version must be x.y.z')

    set_version(a.version)
    if not a.skip_build:
        failed = [mid for mid, m in MODELS.items() if run_env(m['env']) != 0]
        if failed:
            sys.exit('build FAILED for: ' + ', '.join(failed) + ' — nothing staged')

    rel_dir = os.path.join(ROOT, 'firmware', a.version)
    os.makedirs(rel_dir, exist_ok=True)
    release = {'version': a.version, 'build': datetime.date.today().strftime('%Y%m%d'),
               'notes': a.notes[:120], 'images': {}}
    for mid, m in MODELS.items():
        built = image_path(m['env'])
        data = open(built, 'rb').read()
        check_image(mid, m, data, a.version)
        path = '%s/%s.bin' % (a.version, mid)
        shutil.copyfile(built, os.path.join(ROOT, 'firmware', path))
        release['images'][mid] = {'name': m['name'], 'chip': m['chip'], 'path': path, 'size': len(data),
                                  'sha256': hashlib.sha256(data).hexdigest()}
        # For a USB flash (tools/update_tool): the model's bootloader + partition
        # table, so a unit on an older table is brought up to date in one go.
        bdir = os.path.dirname(built)
        for part, fname in (('bootloader', 'bootloader.bin'), ('partitions', 'partitions.bin')):
            blob = open(os.path.join(bdir, fname), 'rb').read()
            rel = '%s/%s.%s.bin' % (a.version, mid, part)
            open(os.path.join(ROOT, 'firmware', rel), 'wb').write(blob)
            release['images'][mid][part] = rel
            release['images'][mid][part + '_sha256'] = hashlib.sha256(blob).hexdigest()
        release['images'][mid]['bootloader_addr'] = BOOTLOADER_ADDR[m['chip']]
        # Legacy per-model channel for units still on 3.x.
        legacy = {'model': mid, 'version': a.version, 'build': release['build']}
        for key, base in URL_BASES.items():
            legacy[key] = '%sfirmware/%s' % (base, path)
        legacy['notes'] = release['notes']
        ch = os.path.join(ROOT, 'firmware', mid)
        os.makedirs(ch, exist_ok=True)
        with open(os.path.join(ch, 'version.json'), 'w', encoding='utf-8') as f:
            json.dump(legacy, f, ensure_ascii=False, indent=2)
            f.write('\n')
        print('  %-11s %-8s %8d B  %s' % (mid, m['chip'], len(data), release['images'][mid]['sha256'][:16]))

    shutil.copyfile(BOOT_APP0, os.path.join(rel_dir, 'boot_app0.bin'))  # otadata image (0xe000)
    release['boot_app0'] = '%s/boot_app0.bin' % a.version
    rj = os.path.join(ROOT, 'firmware', 'release.json')
    with open(rj, 'w', encoding='utf-8') as f:
        json.dump(release, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print('staged firmware/%s + firmware/release.json' % a.version)

    if a.upload_pi:
        def ssh(cmd):
            subprocess.run(['ssh', '-o', 'BatchMode=yes', PI_HOST, cmd], check=True)

        def scp(src, dst):
            subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', src, '%s:%s' % (PI_HOST, dst)], check=True)
        ssh('mkdir -p %s/firmware/%s %s' % (PI_WEBROOT, a.version,
                                            ' '.join('%s/firmware/%s' % (PI_WEBROOT, mid) for mid in MODELS)))
        for f in sorted(os.listdir(rel_dir)):
            scp(os.path.join(rel_dir, f), '%s/firmware/%s/%s' % (PI_WEBROOT, a.version, f))
        for mid in MODELS:
            scp(os.path.join(ROOT, 'firmware', mid, 'version.json'), '%s/firmware/%s/version.json' % (PI_WEBROOT, mid))
        scp(rj, '%s/firmware/release.json' % PI_WEBROOT)  # last: devices only see a complete release
        print('uploaded to %s:%s/firmware' % (PI_HOST, PI_WEBROOT))
    print('next: review, then git add firmware src/core/Version.h && commit && push')


if __name__ == '__main__':
    main()
