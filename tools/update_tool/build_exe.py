"""Build VIETHUD_UPDATE_TOOL.exe (one file, windowed) into the repo root.

    python tools/update_tool/build_exe.py

The exe carries esptool + pyserial + push_data, so updating a unit works on any
Windows PC; building/publishing features look for the VietHUD checkout next to
the exe (or above it) and are disabled without it.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))


def main():
    work = os.path.join(ROOT, '.pio', 'pyinstaller')
    cmd = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--onefile', '--windowed',
           '--name', 'VIETHUD_UPDATE_TOOL',
           '--distpath', ROOT, '--workpath', work, '--specpath', work,
           '--paths', os.path.join(ROOT, 'tools'),
           '--hidden-import', 'push_data',
           '--collect-submodules', 'esptool',
           '--collect-data', 'esptool',
           '--hidden-import', 'serial.tools.list_ports',
           os.path.join(HERE, 'viethud_update_tool.py')]
    print('>', ' '.join(cmd))
    subprocess.run(cmd, check=True)
    print('built', os.path.join(ROOT, 'VIETHUD_UPDATE_TOOL.exe'))


if __name__ == '__main__':
    main()
