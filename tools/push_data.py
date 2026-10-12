"""Push a signed data set to a VietHUD over Wi-Fi (the Phone Update Bridge API).

    python tools/push_data.py                         # find the unit on the LAN, push speedmap/
    python tools/push_data.py --host 192.168.1.254    # a known address
    python tools/push_data.py --dir speedmap_lite154  # another data channel

The unit must have Wi-Fi on (Settings > WiFi) and be on the same network as
this PC (or the PC joined its own access point: --host 192.168.4.1).
The unit checks the manifest signature, takes only the files that differ from
its card, verifies every SHA-256, and installs them on the restart this tool
triggers — rolling back by itself if the new set doesn't mount.
"""
import argparse
import concurrent.futures
import json
import os
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
CHUNK = 1436 * 128  # multiple of the device's 1436-byte read size, < chunkMax 256 KB
HDR = {'X-VietHUD': '1'}


def req(host, method, path, body=None, headers=None, timeout=30):
    r = urllib.request.Request('http://%s%s' % (host, path), data=body, method=method,
                               headers=dict(HDR, **(headers or {})))
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read() or b'null')
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b'null')
        except ValueError:
            return e.code, None
    except (urllib.error.URLError, ConnectionError, TimeoutError, OSError) as e:
        return -1, {'net': str(e)}  # Wi-Fi hiccup: caller re-reads the session and resumes


def find_unit(prefix):
    def ping(ip):
        try:
            with urllib.request.urlopen('http://%s/api/v1/ping' % ip, timeout=1.5) as r:
                j = json.loads(r.read())
                return (ip, j.get('id', '')) if j.get('ok') and 'fw' in j else None
        except Exception:
            return None
    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        hits = [ip for ip in ex.map(ping, ['%s.%d' % (prefix, i) for i in range(1, 255)]) if ip]
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host')
    ap.add_argument('--subnet', default='192.168.1', help='LAN prefix to scan when --host is not given')
    ap.add_argument('--id', help='unit id (last 4 hex of its MAC, shown by the portal) when several are online')
    ap.add_argument('--dir', default='speedmap')
    a = ap.parse_args()

    host = a.host
    if not host:
        hits = [h for h in find_unit(a.subnet) if not a.id or h[1].upper() == a.id.upper()]
        if not hits:
            sys.exit('no VietHUD%s found on %s.x — turn its Wi-Fi on (Settings > WiFi)' %
                     (' ' + a.id if a.id else '', a.subnet))
        if len(hits) > 1:
            sys.exit('several units online, pick one with --id: %s' % ', '.join('%s (%s)' % (i, ip) for ip, i in hits))
        host = hits[0][0]
    st, state = req(host, 'GET', '/api/v1/update/state')
    dev = state['device']
    print('unit %s at %s, fw %s, card ok=%s free=%s MB, data %s' % (dev['id'], host, dev['fw'], state['data']['sd']['ok'],
          state['data']['sd']['freeMB'], state['data']['local']['version'] or '(none)'))

    d = os.path.join(ROOT, a.dir)
    manifest = open(os.path.join(d, 'manifest.txt'), 'rb').read()
    sig = open(os.path.join(d, 'manifest.txt.sig'), 'rb').read().strip()
    st, j = req(host, 'POST', '/api/v1/update/session', b'sig ' + sig + b'\n' + manifest, {'Content-Type': 'text/plain'})
    if st == 200:
        print('already up to date (%s)' % manifest.split(b'\n', 1)[0].decode())
        return
    if st != 201:
        sys.exit('session refused: %s %s' % (st, j))
    sess = j['session']
    sid = sess['sid']
    need = [f for f in sess['files'] if f['r'] < f['s']]
    total = sum(f['s'] - f['r'] for f in need)
    print('release %s: sending %d file(s), %.1f MB' % (sess['version'], len(need), total / 1e6))
    t0, sent = time.time(), 0
    for f in need:
        path = os.path.join(d, f['n'])
        off = f['r']
        with open(path, 'rb') as fh:
            while off < f['s']:
                fh.seek(off)
                chunk = fh.read(CHUNK)
                for attempt in range(20):
                    st, j = req(host, 'PUT', '/api/v1/update/file', chunk,
                                {'X-Sid': sid, 'X-Name': f['n'], 'X-Offset': str(off),
                                 'Content-Type': 'application/octet-stream'}, timeout=60)
                    if st == 200:
                        off = j['received']
                        break
                    if st == 409 and j and 'expected' in j:  # resume from what the card really has
                        off = j['expected']
                        break
                    print('\n  retry %s @%d: %s %s' % (f['n'], off, st, j))
                    time.sleep(3)
                    if st == -1:  # connection dropped: ask the unit what it really has
                        gs, gj = req(host, 'GET', '/api/v1/update/session')
                        if gs == 200 and gj and gj.get('session'):
                            for g in gj['session']['files']:
                                if g['n'] == f['n']:
                                    off = g['r']
                        fh.seek(off)
                        chunk = fh.read(CHUNK)
                else:
                    sys.exit('upload failed')
                sent += len(chunk)
                el = time.time() - t0
                print('\r  %-16s %5.1f%%  %.0f KB/s   ' % (f['n'], 100.0 * off / f['s'], sent / 1024 / max(el, 0.1)),
                      end='', flush=True)
        print()
    st, j = req(host, 'POST', '/api/v1/update/commit', b'', {'X-Sid': sid}, timeout=120)
    if st != 200:
        sys.exit('commit refused: %s %s' % (st, j))
    print('committed — the unit restarts and installs the data')


if __name__ == '__main__':
    main()
