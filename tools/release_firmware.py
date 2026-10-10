"""Build and stage a VietHUD firmware release for one model's OTA channel.

    python tools/release_firmware.py viethud28 3.1.2 --notes "Fix GPS watchdog reboot"
    python tools/release_firmware.py viethud28 3.1.2 --notes "..." --upload-pi

What it does:
  1. builds the PlatformIO env (version stamped with -DVIETHUD_FW_VERSION);
  2. copies firmware.bin to firmware/<channel>/<version>/firmware.bin;
  3. writes firmware/<channel>/version.json with the "model" field the
     firmware requires (core/Version.h VIETHUD_MODEL_ID) and one download URL
     per route the device tries (Pi LAN, mDNS, Tailscale, GitHub);
  4. with --upload-pi, copies both files to the Pi web root over ssh.
Committing / pushing the release folder to GitHub stays a manual, reviewed
step (it is what devices outside the home network download from).

Every model has its own channel and version line — never publish one model's
image into another's folder (the firmware refuses a version.json whose
"model" isn't its own, but a wrong folder still blocks updates for that model).
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

# env -> (model id, channel folder). Must match src/core/Version.h.
MODELS = {
    'viethud': ('viethud35', 'firmware/viethud35'),
    'viethud28': ('viethud28', 'firmware/viethud28'),
}
PI_HOST = 'admin@100.107.34.92'
PI_WEBROOT = '/var/www/viethud'
URL_BASES = {
    'url_pi4': 'http://192.168.1.65/viethud/',
    'url_pi4_mdns': 'http://homebridge.local/viethud/',
    'url_pi4_tailscale': 'http://100.107.34.92/viethud/',
    'url_github': 'https://raw.githubusercontent.com/911273/VietHUD/main/',
}


def run(cmd, **kw):
    print('>', ' '.join(cmd))
    subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('env', choices=sorted(MODELS))
    ap.add_argument('version', help='x.y.z, must be newer than what devices run')
    ap.add_argument('--notes', default='', help='release notes shown in the update prompt (<=120 chars)')
    ap.add_argument('--upload-pi', action='store_true', help='also copy the release to the Pi web root')
    a = ap.parse_args()

    model, channel = MODELS[a.env]
    env = dict(os.environ, PLATFORMIO_BUILD_FLAGS='-DVIETHUD_FW_VERSION=\\"%s\\"' % a.version)
    run(['pio', 'run', '-e', a.env], cwd=ROOT, env=env)

    built = os.path.join(ROOT, '.pio', 'build', a.env, 'firmware.bin')
    data = open(built, 'rb').read()
    if a.version.encode() not in data:
        sys.exit('built image does not contain version %s — refusing to stage it' % a.version)

    rel_dir = os.path.join(ROOT, channel, a.version)
    os.makedirs(rel_dir, exist_ok=True)
    shutil.copyfile(built, os.path.join(rel_dir, 'firmware.bin'))
    manifest = {'model': model, 'version': a.version, 'build': datetime.date.today().strftime('%Y%m%d')}
    for key, base in URL_BASES.items():
        manifest[key] = '%s%s/%s/firmware.bin' % (base, channel, a.version)
    manifest['notes'] = a.notes[:120]
    vj = os.path.join(ROOT, channel, 'version.json')
    with open(vj, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print('staged %s (%d bytes, sha256 %s)' % (rel_dir, len(data), hashlib.sha256(data).hexdigest()[:16]))

    if a.upload_pi:
        remote = '%s/%s' % (PI_WEBROOT, channel)
        run(['ssh', '-o', 'BatchMode=yes', PI_HOST, 'mkdir -p %s/%s' % (remote, a.version)])
        run(['scp', '-q', '-o', 'BatchMode=yes', os.path.join(rel_dir, 'firmware.bin'),
             '%s:%s/%s/firmware.bin' % (PI_HOST, remote, a.version)])
        run(['scp', '-q', '-o', 'BatchMode=yes', vj, '%s:%s/version.json' % (PI_HOST, remote)])
        print('uploaded to %s:%s' % (PI_HOST, remote))
    print('next: review, then git add %s && commit && push (devices away from home update from GitHub)' % channel)


if __name__ == '__main__':
    main()
