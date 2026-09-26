# -*- coding: utf-8 -*-
"""
vhpaths — shared paths, .env loading and logging for the VietHUD data pipeline.

Layout (root overridable via env VIETHUD_PIPELINE_HOME; default = this file's dir):
  <HOME>/
    fetch_osm.py, synthesize_and_pack.py, github_and_notify.py, run_pipeline.py
    vhpack/                     package with the binary format packers
    .env                        secrets/config (NOT committed) — see .env.example
    data/
      osm/vietnam-latest.osm.pbf
      osm/fetch_state.json
      state/                    last-manifest hashes etc.
    input/
      custom_alerts/*.csv       operator's field-surveyed alerts (dropped in here)
    build/speedmap/             freshly packed output for this run
    repo/                       git clone of github.com/911273/VietHUD (push target)
    logs/pipeline.log
"""
import logging
import os
from logging.handlers import RotatingFileHandler
from pathlib import Path

HOME = Path(os.environ.get("VIETHUD_PIPELINE_HOME", Path(__file__).resolve().parent))

DATA_DIR = HOME / "data"
INPUT_DIR = HOME / "input"
CUSTOM_ALERTS_DIR = INPUT_DIR / "custom_alerts"
BUILD_DIR = HOME / "build"
SPEEDMAP_OUT = BUILD_DIR / "speedmap"
REPO_DIR = HOME / "repo"
STATE_DIR = DATA_DIR / "state"
LOG_DIR = HOME / "logs"

for d in (DATA_DIR, CUSTOM_ALERTS_DIR, SPEEDMAP_OUT, STATE_DIR, LOG_DIR):
    d.mkdir(parents=True, exist_ok=True)


def _load_dotenv():
    """Minimal .env loader (KEY=VALUE lines) — avoids a python-dotenv dependency.
    Does not overwrite variables already set in the real environment."""
    env_path = HOME / ".env"
    if not env_path.exists():
        return
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip().strip('"').strip("'")
        os.environ.setdefault(key, val)


_load_dotenv()


def _make_logger():
    lg = logging.getLogger("viethud")
    if lg.handlers:
        return lg
    lg.setLevel(logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s",
                            "%Y-%m-%d %H:%M:%S")
    # stdout -> captured by systemd journald
    sh = logging.StreamHandler()
    sh.setFormatter(fmt)
    lg.addHandler(sh)
    # rotating file as a fallback (logrotate also manages this path); small +
    # few backups to spare the SD card.
    try:
        fh = RotatingFileHandler(LOG_DIR / "pipeline.log", maxBytes=1_000_000,
                                 backupCount=3, encoding="utf-8")
        fh.setFormatter(fmt)
        lg.addHandler(fh)
    except Exception:
        pass
    return lg


LOG = _make_logger()

# Config from environment (.env)
GIT_REMOTE = os.environ.get("VIETHUD_GIT_REMOTE",
                            "https://github.com/911273/VietHUD.git")
GIT_BRANCH = os.environ.get("VIETHUD_GIT_BRANCH", "main")
GIT_SPEEDMAP_SUBDIR = os.environ.get("VIETHUD_GIT_SPEEDMAP_SUBDIR", "speedmap")
TELEGRAM_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
TELEGRAM_CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")
RAW_MANIFEST_URL = os.environ.get(
    "VIETHUD_RAW_MANIFEST_URL",
    "https://raw.githubusercontent.com/911273/VietHUD/main/speedmap/manifest.txt")
REGION_CODE = os.environ.get("VIETHUD_REGION", "VN")
