"""Build + sign + (optionally) publish the road-name channels — OpenStreetMap only.

    python tools/map_builder/publish_roadnames.py                       # from data/speedmap/roads.sqlite
    python tools/map_builder/publish_roadnames.py --pbf vietnam-latest.osm.pbf --push   # the Pi's weekly job

Channels (each: roadnames.bin + manifest.txt + manifest.txt.sig, read by the
firmware's net/NamesUpdater.cpp):
  roadnames/          whole country, every named road incl. alleys (~14 MB) — card boards
  roadnames_lite154/  LITE_BBOX around Ha Noi (~1.6 MB) — fits the Lite's flash

A channel is only re-published when its roadnames.bin actually changed, so
devices don't re-download 14 MB for nothing. --push commits ONLY these two
folders and pushes (rebasing over whatever else landed on main meanwhile).
"""
import argparse
import datetime
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_road_names as B  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
HOME = (21.0285, 105.8542)  # Ha Noi
LITE_BBOX = (HOME[0] - 0.3, HOME[0] + 0.3, HOME[1] - 0.35, HOME[1] + 0.3)  # ~33 km: all of Ha Noi proper
CHANNELS = {'roadnames': None, 'roadnames_lite154': LITE_BBOX}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def current_sha(channel_dir):
    try:
        lines = open(os.path.join(channel_dir, 'manifest.txt'), encoding='utf-8').read().split('\n')
        return lines[1].split()[2]
    except (OSError, IndexError):
        return None


def publish_channel(repo, name, bbox, src_db, pbf, version):
    d = os.path.join(repo, name)
    os.makedirs(d, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'roadnames.bin')
        B.build(src_db, out, bbox, pbf=pbf)
        new_sha = sha256(out)
        if new_sha == current_sha(d) and os.path.exists(os.path.join(d, 'manifest.txt.sig')):
            print('[%s] unchanged — not re-published' % name)
            return False
        shutil.copyfile(out, os.path.join(d, 'roadnames.bin'))
    size = os.path.getsize(os.path.join(d, 'roadnames.bin'))
    with open(os.path.join(d, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('version %s\nroadnames.bin %d %s\n' % (version, size, new_sha))
    signer = os.path.join(HERE, '..', 'sign_manifest.py')
    subprocess.run([sys.executable, signer, os.path.join(d, 'manifest.txt')], check=True)
    print('[%s] published %s (%.2f MB)' % (name, version, size / 1e6))
    return True


def git(repo, *args, check=True):
    return subprocess.run(['git', '-C', repo] + list(args), check=check, capture_output=True, text=True)


def push(repo, version):
    git(repo, 'add', '--', *CHANNELS.keys())
    if not git(repo, 'diff', '--cached', '--quiet', check=False).returncode:
        print('nothing to commit')
        return
    git(repo, 'commit', '-m', 'auto: road-name channels %s (OpenStreetMap)' % version, '--', *CHANNELS.keys())
    for attempt in range(3):
        git(repo, 'pull', '--rebase', '-q', 'origin', 'main', check=False)
        r = git(repo, 'push', '-q', 'origin', 'HEAD:main', check=False)
        if r.returncode == 0:
            print('pushed')
            return
        print('push failed (%s), retrying' % r.stderr.strip())
    sys.exit('push failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--db', default=B.DB)
    ap.add_argument('--pbf')
    ap.add_argument('--repo', default=ROOT, help='repo checkout to write the channel folders into')
    ap.add_argument('--push', action='store_true')
    a = ap.parse_args()
    now = datetime.datetime.utcnow() + datetime.timedelta(hours=7)
    version = now.strftime('%Y.%m.%d.%H%M')
    changed = [publish_channel(a.repo, n, bbox, a.db, a.pbf, version) for n, bbox in CHANNELS.items()]
    if a.push and any(changed):
        push(a.repo, version)


if __name__ == '__main__':
    main()
