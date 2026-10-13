#!/usr/bin/env python3
"""VietHUD Update Tool — update a unit plugged in over USB, and publish releases.

    python tools/update_tool/viethud_update_tool.py          # window
    python tools/update_tool/viethud_update_tool.py --cli identify COM10
    python tools/update_tool/viethud_update_tool.py --cli firmware COM10
    python tools/update_tool/viethud_update_tool.py --cli data COM10

Device (USB):
  * Identify: reads the partition table, the OTA state and the running image
    over the ROM bootloader (esptool) and finds the model + version markers the
    firmware carries ("VIETHUD-HW-ID:<model>;", "VIETHUD-FW:<ver>;"). A unit on
    old firmware without markers: pick the model by hand.
  * Firmware: downloads firmware/release.json (Pi on the LAN first, then GitHub),
    checks SHA-256, writes the model's image to app0 and selects it. When the
    unit's partition table differs from the release's (older layout), the
    bootloader + table are written too — the data partition then has to be
    re-filled (Lite/P4: "Data" below; NVS settings are kept).
  * Data: flash-data boards (Lite 1.54", P4 without card) get their flash image
    built from this checkout and written over USB (needs PlatformIO); card
    boards get the signed data set pushed over Wi-Fi (the unit must be on the
    same network — tools/push_data.py).
Publish (needs this repo): firmware release for every model (tools/release.py
--upload-pi + git push), and SpeedMap Studio for map/alert data.

Build the .exe:  python tools/update_tool/build_exe.py
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
from contextlib import redirect_stdout, redirect_stderr

HERE = os.path.dirname(os.path.abspath(sys.argv[0] if getattr(sys, 'frozen', False) else __file__))


def find_repo():
    """The VietHUD checkout (for build/publish features): next to the exe/script or above it."""
    d = HERE
    for _ in range(5):
        if os.path.exists(os.path.join(d, 'tools', 'models.py')) and os.path.exists(os.path.join(d, 'platformio.ini')):
            return d
        d = os.path.dirname(d)
    return None


REPO = find_repo()
if REPO:
    sys.path.insert(0, os.path.join(REPO, 'tools'))

SOURCES = [
    ('Pi 4 (LAN)', 'http://192.168.1.65/viethud/'),
    ('Pi 4 (Tailscale)', 'http://100.107.34.92/viethud/'),
    ('GitHub', 'https://raw.githubusercontent.com/911273/VietHUD/main/'),
]
MODEL_NAMES = {'viethud35': 'VietHUD 3.5"', 'viethud28': 'VietHUD 2.8"', 'viethud154': 'VietHUD Lite 1.54"',
               'viethud43': 'VietHUD 4.3" (P4)'}
MODEL_ENV = {'viethud35': 'viethud', 'viethud28': 'viethud28', 'viethud154': 'viethud_lite154',
             'viethud43': 'viethud_p4'}
FLASH_DATA_MODELS = {'viethud154': 'lite154', 'viethud43': 'p4'}
CHIP_ARG = {'esp32s3': 'esp32s3', 'esp32p4': 'esp32p4'}
CACHE = os.path.join(tempfile.gettempdir(), 'viethud_update_tool')
os.makedirs(CACHE, exist_ok=True)

LOG = print  # replaced by the GUI


# ---------------------------------------------------------------- esptool
def esptool_run(args):
    """Runs esptool in-process (works inside the .exe too); returns its output."""
    import esptool
    buf = io.StringIO()
    try:
        with redirect_stdout(buf), redirect_stderr(buf):
            esptool.main(args)
    except SystemExit as e:
        if e.code not in (0, None):
            raise RuntimeError(buf.getvalue()[-800:])
    except Exception as e:  # esptool.FatalError and friends
        raise RuntimeError('%s\n%s' % (e, buf.getvalue()[-800:]))
    return buf.getvalue()


def list_ports():
    from serial.tools import list_ports as lp
    out = []
    for p in lp.comports():
        vid = p.vid or 0
        esp = vid in (0x303A, 0x10C4, 0x1A86, 0x0403)  # Espressif USB, CP210x, CH34x, FTDI
        out.append((p.device, p.description, esp))
    out.sort(key=lambda x: (not x[2], x[0]))
    return out


def parse_partitions(tbl):
    parts = {}
    for i in range(0, len(tbl) - 31, 32):
        e = tbl[i:i + 32]
        if e[:2] != b'\xaa\x50':
            continue
        name = e[12:28].split(b'\0')[0].decode(errors='replace')
        parts[name] = dict(type=e[2], subtype=e[3], offset=int.from_bytes(e[4:8], 'little'),
                           size=int.from_bytes(e[8:12], 'little'))
    return parts


def identify(port, chip='auto'):
    """Model, version, chip, MAC, partition table and active slot of the plugged-in unit."""
    head = os.path.join(CACHE, 'head.bin')
    LOG('[usb] reading partition table + OTA state on %s ...' % port)
    out = esptool_run(['--chip', chip, '--port', port, '-b', '921600', 'read-flash', '0x8000', '0x8000', head])
    chip_found = 'esp32p4' if 'ESP32-P4' in out else 'esp32s3' if 'ESP32-S3' in out else None
    mac = (re.search(r'MAC:\s*([0-9a-fA-F:]{17})', out) or [None, ''])[1]
    blob = open(head, 'rb').read()
    tbl = blob[:0xC00]
    parts = parse_partitions(tbl)
    ota = next((p for p in parts.values() if p['type'] == 1 and p['subtype'] == 0), None)
    slot = 0
    if ota and 0x8000 <= ota['offset'] < 0xF000:
        o = blob[ota['offset'] - 0x8000: ota['offset'] - 0x8000 + 0x2000]
        seqs = [int.from_bytes(o[k * 0x1000:k * 0x1000 + 4], 'little') for k in range(2)]
        valid = [s for s in seqs if s not in (0, 0xFFFFFFFF)]
        if valid:
            slot = (max(valid) - 1) % 2
    app = parts.get('app%d' % slot) or parts.get('app0')
    model = ver = None
    if app:
        img = os.path.join(CACHE, 'app.bin')
        LOG('[usb] reading the running firmware (app%d) ...' % slot)
        esptool_run(['--chip', chip_found or chip, '--port', port, '-b', '921600', 'read-flash',
                     hex(app['offset']), hex(min(app['size'], 0x80000)), img])
        data = open(img, 'rb').read()
        m = re.search(rb'VIETHUD-HW-ID:([a-z0-9]+);', data)
        v = re.search(rb'VIETHUD-FW:([0-9.]+);', data)
        model = m.group(1).decode() if m else None
        ver = v.group(1).decode() if v else None
        if not ver:  # pre-marker firmware: best effort
            v = re.search(rb'(?<![0-9.])([34]\.[0-9]+\.[0-9]+)(?![0-9.])', data)
            ver = (v.group(1).decode() + '?') if v else None
    info = dict(port=port, chip=chip_found, mac=mac, unit_id=mac.replace(':', '')[-4:].upper() if mac else '',
                model=model, version=ver, slot=slot, partitions=parts, table=tbl)
    LOG('[usb] %s' % describe(info))
    return info


def describe(info):
    return '%s | model %s | firmware %s | chip %s | id %s' % (
        info['port'], MODEL_NAMES.get(info['model'], info['model'] or 'unknown'), info['version'] or '?',
        info['chip'], info['unit_id'])


# ---------------------------------------------------------------- release
def http_get(url, timeout=15):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def fetch_release():
    for name, base in SOURCES:
        try:
            rel = json.loads(http_get(base + 'firmware/release.json', 6))
            LOG('[release] %s: firmware %s' % (name, rel['version']))
            return rel, base, name
        except Exception as e:
            LOG('[release] %s not reachable (%s)' % (name, str(e)[:60]))
    raise RuntimeError('No release source reachable (Pi or GitHub)')


def fetch_file(base, rel_path, sha=None):
    dst = os.path.join(CACHE, rel_path.replace('/', '_'))
    if not (os.path.exists(dst) and sha and hashlib.sha256(open(dst, 'rb').read()).hexdigest() == sha):
        LOG('[release] downloading %s ...' % rel_path)
        data = http_get(base + 'firmware/' + rel_path, 120)
        if sha and hashlib.sha256(data).hexdigest() != sha:
            raise RuntimeError('SHA-256 mismatch for %s' % rel_path)
        open(dst, 'wb').write(data)
    return dst


def flash_firmware(info, model=None):
    model = model or info['model']
    if not model:
        raise RuntimeError('Unknown model — choose it in the list')
    rel, base, src = fetch_release()
    img = rel['images'].get(model)
    if not img:
        raise RuntimeError('Release %s has no image for %s' % (rel['version'], model))
    if info['chip'] and img['chip'] != info['chip']:
        raise RuntimeError('Image is for %s but the unit is %s' % (img['chip'], info['chip']))
    app = fetch_file(base, img['path'], img['sha256'])
    boot0 = fetch_file(base, rel['boot_app0']) if rel.get('boot_app0') else None
    parts = None
    if img.get('partitions'):
        parts = fetch_file(base, img['partitions'], img.get('partitions_sha256'))
    new_tbl = open(parts, 'rb').read() if parts else None
    full = bool(new_tbl) and new_tbl[:0xC00].rstrip(b'\xff') != info['table'].rstrip(b'\xff')
    new_parts = parse_partitions(new_tbl) if new_tbl else info['partitions']
    app0 = new_parts['app0']['offset']
    ota = next(p for p in new_parts.values() if p['type'] == 1 and p['subtype'] == 0)
    args = ['--chip', img['chip'], '--port', info['port'], '-b', '921600', 'write-flash', '-z']
    if full:
        boot = fetch_file(base, img['bootloader'], img.get('bootloader_sha256'))
        LOG('[flash] partition table differs from the release: writing bootloader + table too')
        args += [hex(img.get('bootloader_addr', 0)), boot, '0x8000', parts]
    if boot0:
        args += [hex(ota['offset']), boot0]
    args += [hex(app0), app]
    LOG('[flash] writing %s %s (%s) ...' % (MODEL_NAMES.get(model, model), rel['version'], src))
    esptool_run(args)
    LOG('[flash] done — the unit restarts into %s' % rel['version'])
    return full


# ---------------------------------------------------------------- data
def update_data(info, model=None):
    model = model or info['model']
    if model in FLASH_DATA_MODELS:
        if not REPO:
            raise RuntimeError('Flash data needs the VietHUD checkout + PlatformIO on this PC')
        prof = FLASH_DATA_MODELS[model]
        src = flash_dataset()
        LOG('[data] cutting the %s flash image from %s ...' % (prof, src))
        run([sys.executable if not getattr(sys, 'frozen', False) else 'python',
             os.path.join(REPO, 'tools', 'build_flash_data.py'), prof, '--src', src])
        LOG('[data] writing it over USB (PlatformIO uploadfs) ...')
        run(['python', os.path.join(REPO, 'tools', 'build.py'), MODEL_ENV[model], 'uploadfs', '--port', info['port']])
        LOG('[data] done')
        return
    # card board: push the signed set over Wi-Fi
    import push_data
    LOG('[data] %s keeps its data on the microSD card: pushing over Wi-Fi.' % MODEL_NAMES.get(model, model))
    LOG('[data] turn Wi-Fi on on the unit (Settings > WiFi) — looking for id %s on the LAN ...' % info['unit_id'])
    d = download_speedmap()
    for _ in range(60):
        hits = [h for h in push_data.find_unit('192.168.1') if h[1].upper() == info['unit_id']]
        if hits:
            break
        time.sleep(10)
    else:
        raise RuntimeError('Unit %s did not show up on the Wi-Fi network' % info['unit_id'])
    old = sys.argv
    sys.argv = ['push_data.py', '--host', hits[0][0], '--dir', d]
    try:
        push_data.main()
    finally:
        sys.argv = old


DATA_SOURCES = SOURCES[:2]  # data lives on the Pi only (LAN at home, Tailscale away)


def download_speedmap(channel='speedmap'):
    """The signed data set from the Pi (http://<pi>/viethud/<channel>/), cached;
    files already in the cache with the right SHA-256 are not fetched again."""
    d = os.path.join(CACHE, channel)
    os.makedirs(d, exist_ok=True)
    last = None
    for name, root in DATA_SOURCES:
        base = root + channel + '/'
        try:
            man = http_get(base + 'manifest.txt', 6).decode()
        except Exception as e:
            last = e
            LOG('[data] %s not reachable (%s)' % (name, str(e)[:60]))
            continue
        LOG('[data] %s: %s %s' % (name, channel, man.split('\n', 1)[0]))
        sig = http_get(base + 'manifest.txt.sig')
        for line in man.strip().split('\n')[1:]:
            fname, size, sha = line.split()
            fp = os.path.join(d, fname)
            if os.path.exists(fp) and file_sha256(fp) == sha:
                continue
            LOG('[data] downloading %s (%.1f MB) ...' % (fname, int(size) / 1e6))
            for attempt in range(5):  # a dropped connection resumes where it stopped
                have = os.path.getsize(fp + '.part') if attempt and os.path.exists(fp + '.part') else 0
                req = urllib.request.Request(base + fname, headers={'Range': 'bytes=%d-' % have} if have else {})
                try:
                    with urllib.request.urlopen(req, timeout=60) as r, open(fp + '.part', 'ab' if have else 'wb') as f:
                        if have and r.status != 206:
                            f.truncate(0)
                        shutil.copyfileobj(r, f, 1 << 20)
                except Exception as e:
                    LOG('[data] %s: %s — retrying' % (fname, str(e)[:60]))
                if os.path.getsize(fp + '.part') >= int(size):
                    break
            if file_sha256(fp + '.part') != sha:
                os.remove(fp + '.part')
                raise RuntimeError('SHA-256 mismatch for %s' % fname)
            os.replace(fp + '.part', fp)
        open(os.path.join(d, 'manifest.txt'), 'w', newline='\n').write(man)
        open(os.path.join(d, 'manifest.txt.sig'), 'wb').write(sig)
        return d
    raise RuntimeError('Data server (Pi) not reachable: %s' % last)


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def flash_dataset():
    """The main-road set flash images are cut from: SpeedMap Studio's flash set
    on this PC when there is one, else the Lite channel published on the Pi."""
    if REPO:
        work = os.path.join(REPO, 'data', 'studio', 'flash', 'speedmap')
        if os.path.exists(os.path.join(work, 'metadata.bin')):
            return work
    return download_speedmap('speedmap_lite154')


# ---------------------------------------------------------------- publish
def run(cmd, cwd=None):
    LOG('> ' + ' '.join(cmd))
    p = subprocess.Popen(cmd, cwd=cwd or REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding='utf-8', errors='replace')
    for line in p.stdout:
        line = line.rstrip()
        if line and not line.startswith(('Compiling', 'Archiving', 'Linking', 'Indexing')):
            LOG(line)
    if p.wait() != 0:
        raise RuntimeError('command failed: %s' % ' '.join(cmd[:3]))


def publish_firmware(version, notes):
    if not REPO:
        raise RuntimeError('Publishing needs the VietHUD checkout')
    run(['python', os.path.join(REPO, 'tools', 'release.py'), version, '--notes', notes, '--upload-pi'])
    run(['git', 'add', 'firmware', 'src/core/Version.h'])
    run(['git', 'commit', '-m', 'Release %s for every model\n\n%s' % (version, notes)])
    run(['git', 'pull', '--rebase', '--autostash', 'origin', 'main'])
    run(['git', 'push', 'origin', 'HEAD:main'])
    LOG('[publish] firmware %s is on GitHub and the Pi' % version)


def next_version():
    if not REPO:
        return ''
    s = open(os.path.join(REPO, 'src', 'core', 'Version.h'), encoding='utf-8').read()
    m = re.search(r'VIETHUD_FW_VERSION "(\d+)\.(\d+)\.(\d+)"', s)
    return '%s.%s.%d' % (m.group(1), m.group(2), int(m.group(3)) + 1) if m else ''


def open_studio():
    if not REPO:
        raise RuntimeError('SpeedMap Studio needs the VietHUD checkout')
    subprocess.Popen(['python', os.path.join(REPO, 'tools', 'speedmap_studio', 'server.py')],
                     cwd=os.path.join(REPO, 'tools', 'speedmap_studio'),
                     creationflags=getattr(subprocess, 'CREATE_NEW_CONSOLE', 0))
    time.sleep(2)
    webbrowser.open('http://localhost:8088')


# ---------------------------------------------------------------- GUI
def gui():
    import tkinter as tk
    from tkinter import ttk, messagebox, simpledialog

    root = tk.Tk()
    root.title('VietHUD Update Tool')
    root.geometry('860x620')
    q = queue.Queue()

    def log(msg):
        q.put(str(msg))

    global LOG
    LOG = log
    state = {'info': None, 'busy': False}

    top = ttk.Frame(root, padding=8)
    top.pack(fill='x')
    ttk.Label(top, text='Cổng USB:').pack(side='left')
    port_var = tk.StringVar()
    port_box = ttk.Combobox(top, textvariable=port_var, width=40, state='readonly')
    port_box.pack(side='left', padx=4)

    def refresh_ports():
        ports = list_ports()
        port_box['values'] = ['%s — %s' % (p, d) for p, d, _ in ports]
        if ports:
            port_box.current(0)
    ttk.Button(top, text='↻', width=3, command=refresh_ports).pack(side='left')
    ttk.Label(top, text='  Model:').pack(side='left')
    model_var = tk.StringVar(value='(tự nhận)')
    ttk.Combobox(top, textvariable=model_var, width=22, state='readonly',
                 values=['(tự nhận)'] + ['%s — %s' % (k, v) for k, v in MODEL_NAMES.items()]).pack(side='left', padx=4)

    info_var = tk.StringVar(value='Cắm thiết bị qua USB rồi bấm "Nhận diện".')
    ttk.Label(root, textvariable=info_var, padding=(10, 2), foreground='#0a5').pack(fill='x')

    btns = ttk.Frame(root, padding=8)
    btns.pack(fill='x')
    prog = ttk.Progressbar(root, mode='indeterminate')
    prog.pack(fill='x', padx=8)
    txt = tk.Text(root, height=24, bg='#101418', fg='#d8e0e8', insertbackground='white', font=('Consolas', 9))
    txt.pack(fill='both', expand=True, padx=8, pady=8)

    def port():
        v = port_var.get()
        if not v:
            raise RuntimeError('Chưa chọn cổng USB')
        return v.split(' — ')[0]

    def chosen_model():
        v = model_var.get()
        return None if v.startswith('(') else v.split(' — ')[0]

    def task(fn, done_msg=None):
        if state['busy']:
            return
        state['busy'] = True
        prog.start(12)

        def worker():
            try:
                r = fn()
                if done_msg:
                    log('✔ ' + done_msg)
                return r
            except Exception as e:
                log('✖ LỖI: %s' % e)
            finally:
                q.put(('__done__',))
        threading.Thread(target=worker, daemon=True).start()

    def do_identify():
        info = identify(port())
        state['info'] = info
        q.put(('__info__', describe(info)))

    def need_info():
        if not state['info'] or state['info']['port'] != port():
            do_identify()
        return state['info']

    def do_fw():
        info = need_info()
        full = flash_firmware(info, chosen_model())
        if full and (chosen_model() or info['model']) in FLASH_DATA_MODELS:
            log('Bảng phân vùng đã đổi: hãy bấm "Cập nhật dữ liệu" để nạp lại dữ liệu flash.')

    def do_data():
        update_data(need_info(), chosen_model())

    def do_publish():
        ver = simpledialog.askstring('Phát hành firmware', 'Số phiên bản mới:', initialvalue=next_version())
        if not ver:
            return
        notes = simpledialog.askstring('Phát hành firmware', 'Ghi chú (≤120 ký tự):', initialvalue='') or ''
        if not messagebox.askyesno('Phát hành', 'Build cả 4 model, phát hành %s lên Pi và GitHub?' % ver):
            return
        task(lambda: publish_firmware(ver, notes), 'Đã phát hành firmware %s' % ver)

    for text, cmd in (('🔍 Nhận diện', lambda: task(do_identify)),
                      ('⬆ Cập nhật firmware', lambda: task(do_fw, 'Cập nhật firmware xong')),
                      ('🗺 Cập nhật dữ liệu', lambda: task(do_data, 'Cập nhật dữ liệu xong')),
                      ('🚀 Phát hành firmware', do_publish),
                      ('🧭 SpeedMap Studio', lambda: task(open_studio))):
        ttk.Button(btns, text=text, command=cmd).pack(side='left', padx=4)

    def pump():
        try:
            while True:
                m = q.get_nowait()
                if isinstance(m, tuple):
                    if m[0] == '__done__':
                        state['busy'] = False
                        prog.stop()
                    elif m[0] == '__info__':
                        info_var.set(m[1])
                    continue
                txt.insert('end', m + '\n')
                txt.see('end')
        except queue.Empty:
            pass
        root.after(100, pump)

    refresh_ports()
    log('VietHUD Update Tool — repo: %s' % (REPO or '(không có: chỉ cập nhật thiết bị)'))
    pump()
    root.mainloop()


def cli(a):
    if a.action == 'ports':
        for p in list_ports():
            print(p)
        return
    info = identify(a.port)
    if a.action == 'firmware':
        flash_firmware(info, a.model)
    elif a.action == 'data':
        update_data(info, a.model)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cli', dest='action', choices=['ports', 'identify', 'firmware', 'data'])
    ap.add_argument('port', nargs='?')
    ap.add_argument('--model', choices=sorted(MODEL_NAMES))
    a = ap.parse_args()
    if a.action:
        cli(a)
    else:
        gui()
