"""Build / flash any VietHUD model with the right toolchain.

    python tools/build.py viethud                  # build 3.5"
    python tools/build.py viethud_p4 upload --port COM7
    python tools/build.py all                      # every model

S3 models use the default PlatformIO core (espressif32@7.1.3). The P4 uses the
isolated pioarduino core in ~/.platformio_p4 — installing pioarduino into the
default core would replace the S3's pinned platform — and ESP-IDF tools refuse
to run under Git Bash/MSYS, so the MSYS variables are stripped for it.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from models import MODELS, ROOT  # noqa: E402

P4_CORE = os.path.join(os.path.expanduser('~'), '.platformio_p4')
P4_PIO = os.path.join(P4_CORE, 'penv', 'Scripts', 'pio.exe')


def pio_cmd(env_name, extra_env=None):
    """(argv prefix, environment) for running pio on this env."""
    env = dict(os.environ)
    env.update(extra_env or {})
    is_p4 = env_name.startswith('viethud_p4')
    if is_p4:
        for k in ('MSYSTEM', 'MSYSTEM_PREFIX', 'MSYSTEM_CHOST', 'MSYSTEM_CARCH', 'MINGW_PREFIX', 'SHELL', 'TERM'):
            env.pop(k, None)
        env['PLATFORMIO_CORE_DIR'] = P4_CORE
        # Own build/libdeps dirs: a different core computes a different project
        # checksum and WIPES .pio/build of every other env when they share it
        # (seen 2026-10-12: S3 images vanished, parallel builds broke).
        env['PLATFORMIO_BUILD_DIR'] = os.path.join(ROOT, '.pio_p4', 'build')
        env['PLATFORMIO_LIBDEPS_DIR'] = os.path.join(ROOT, '.pio_p4', 'libdeps')
        return [P4_PIO], env
    # The PlatformIO the user installed (the project .venv doesn't carry it).
    pio = shutil.which('platformio') or shutil.which('pio')
    return ([pio] if pio else [sys.executable, '-m', 'platformio']), env


def image_path(env_name):
    """The app image a build of this env produces."""
    base = '.pio_p4' if env_name.startswith('viethud_p4') else '.pio'
    return os.path.join(ROOT, base, 'build', env_name, 'firmware.bin')


def run_env(env_name, targets=(), port=None, extra_env=None):
    extra_env = dict(extra_env or {})
    if any(t in ('buildfs', 'uploadfs') for t in targets):
        # Flash-data boards: the image comes from data_<profile>/ (tools/build_flash_data.py).
        prof = next((m['data'] for m in MODELS.values() if m['env'] == env_name.replace('_sim', '')), None)
        if not prof:
            sys.exit('%s keeps its data on the microSD card — nothing to upload to flash' % env_name)
        data_dir = os.path.join(ROOT, 'data_' + prof)
        if not os.path.isdir(os.path.join(data_dir, 'speedmap')):
            sys.exit('%s missing — run: python tools/build_flash_data.py %s' % (data_dir, prof))
        extra_env['PLATFORMIO_DATA_DIR'] = data_dir
    argv, env = pio_cmd(env_name, extra_env)
    argv = argv + ['run', '-e', env_name]
    for t in targets:
        argv += ['-t', t]
    if port:
        argv += ['--upload-port', port]
    print('>', ' '.join(argv), flush=True)
    return subprocess.run(argv, cwd=ROOT, env=env).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('env', help='PlatformIO env, or "all"')
    ap.add_argument('targets', nargs='*', help='upload, uploadfs, ...')
    ap.add_argument('--port')
    a = ap.parse_args()
    envs = [m['env'] for m in MODELS.values()] if a.env == 'all' else [a.env]
    failed = [e for e in envs if run_env(e, a.targets, a.port) != 0]
    if failed:
        sys.exit('FAILED: ' + ', '.join(failed))


if __name__ == '__main__':
    main()
