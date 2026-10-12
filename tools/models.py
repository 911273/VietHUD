"""The VietHUD model registry — one place for every tool (build, release, data).

Must agree with include/boards/<board>/board_config.h (BOARD_MODEL_ID) and the
envs in platformio.ini. Adding a board: add it here too.
"""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

# model id -> build facts
#   env       PlatformIO env
#   chip      CONFIG_IDF_TARGET of the image (checked again on the device)
#   p4        needs the isolated pioarduino toolchain (~/.platformio_p4)
#   data      data profile for boards whose data lives in on-chip flash
#             (tools/build_flash_data.py); None = microSD card
MODELS = {
    'viethud35': dict(env='viethud', name='VietHUD 3.5"', chip='esp32s3', p4=False, data=None),
    'viethud28': dict(env='viethud28', name='VietHUD 2.8"', chip='esp32s3', p4=False, data=None),
    'viethud154': dict(env='viethud_lite154', name='VietHUD Lite 1.54"', chip='esp32s3', p4=False, data='lite154'),
    'viethud43': dict(env='viethud_p4', name='VietHUD 4.3" (P4)', chip='esp32p4', p4=True, data='p4'),
}


def platform_version():
    """VIETHUD_FW_VERSION from src/core/Version.h — the one platform version."""
    src = open(os.path.join(ROOT, 'src', 'core', 'Version.h'), encoding='utf-8').read()
    m = re.search(r'#define VIETHUD_FW_VERSION "([0-9.]+)"', src)
    if not m:
        raise SystemExit('VIETHUD_FW_VERSION not found in src/core/Version.h')
    return m.group(1)


def model_for_env(env):
    for mid, m in MODELS.items():
        if m['env'] == env or env.startswith(m['env'] + '_'):
            return mid
    return None
