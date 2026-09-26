# VietHUD Data Pipeline (Raspberry Pi 4 · ODbL)

Automated, self-hosted pipeline that rebuilds the VietHUD firmware data set from
**OpenStreetMap (ODbL)** plus the operator's **own field-surveyed alerts**, then
publishes it to GitHub for the device's Wi-Fi OTA and posts a Telegram summary.

No third-party proprietary data is fetched, decrypted or redistributed — every
source is either open (OSM/ODbL) or produced by the operator (custom CSVs).

---

## 1. Architecture & directory layout (on the Pi)

Install root: **`/opt/viethud-pipeline/`** (on the Pi's SSD root, not the SD card —
see [§6](#6-disk--sd-card)).

```
/opt/viethud-pipeline/
├── venv/                         # Python virtualenv (osmium, requests)
├── vhpaths.py                    # shared paths, .env loader, logging
├── vhpack/formats.py             # binary packers (byte-identical to firmware)
├── fetch_osm.py                  # monthly: download vietnam-latest.osm.pbf
├── synthesize_and_pack.py        # OSM + custom alerts -> build/speedmap/*.bin
├── github_and_notify.py          # sha256 + manifest + git push + Telegram
├── run_pipeline.py               # daily orchestrator (pack -> publish-if-changed)
├── .env                          # secrets/config (chmod 600, NOT committed)
├── assets/sounds/                # Vietnamese MP3 prompts to pack into speedmap/sounds
├── data/
│   ├── osm/vietnam-latest.osm.pbf
│   ├── osm/fetch_state.json      # ETag/md5 for conditional download
│   └── state/                    # last published version/hashes
├── input/
│   └── custom_alerts/*.csv       # <-- drop your surveyed points here
├── build/speedmap/               # freshly packed output of the latest run
├── repo/                         # git clone of github.com/911273/VietHUD (push target)
├── logs/pipeline.log
├── systemd/                      # unit files to install into /etc/systemd/system
└── logrotate/viethud-pipeline    # install into /etc/logrotate.d
```

**Data flow**

```
        (monthly, 01st 13:00)                 (daily, 10:00)
Geofabrik vietnam-latest.osm.pbf  ─┐
                                   ├─> synthesize_and_pack.py ─> build/speedmap/*.bin
input/custom_alerts/*.csv  ────────┘        (spatial dedup ≤10m, heading ≤45°)
                                                    │
                                                    ▼
                                   github_and_notify.py
                                   (sha256 → manifest.txt; publish ONLY if changed)
                                        │                        │
                                        ▼                        ▼
                        GitHub main /speedmap/          Telegram summary
                                        │
                                        ▼
                        Device Wi-Fi OTA (raw.githubusercontent.com/.../manifest.txt)
```

**Outputs** (all Little-Endian, matching `src/map/SpeedMapFormat.h`):
`metadata.bin`, `index.bin`, `tiles.bin` (road network), `cameras.bin`,
`signs.bin` (VNSG v1), `names.bin` + `seg_names.bin` (**V2 / uint32**),
`sounds/`, `manifest.txt`. Raster (`maptiles.bin`) is intentionally excluded.

**Node stitching:** segments are built from the OSM node graph, so every shared
junction node has one canonical coordinate (identical E7 on both sides). That is
exactly what the firmware's RoutePredictor needs — no MBTiles-style tile-clip
gaps, so the firmware can keep a tight node-match tolerance.

---

## 2. What gets extracted from OSM

| VietHUD type | Source |
|---|---|
| Road network + heading + `roadClass` | `highway=*` ways, split per node pair |
| Speed limit (segment) | `maxspeed`, `maxspeed:forward/backward`; else VN class default (TT 31/2019) |
| 🚦 Type 6 traffic light | `highway=traffic_signals` |
| 📷 Type 4 camera | `highway=speed_camera` (also → `cameras.bin`) |
| 🛑 Type 5 toll | `barrier=toll_booth` / `highway=toll_gantry` |
| Street name (V2) | `name:vi` → `name` → `ref`, taken from each way directly |

> OSM `highway=traffic_signals` coverage in Vietnam is uneven (this is why some
> red lights are missing today). Fill gaps with **custom alerts** below and/or by
> contributing back to OSM.

---

## 3. Custom Alerts (your surveyed data)

Drop CSV files into `input/custom_alerts/`. They are merged into `signs.bin`
(and `cameras.bin` for type 4) with spatial de-duplication (≤10 m, heading ≤45°);
your survey point **wins** over an OSM point it duplicates.

CSV header (UTF-8, header row required):

```csv
lat,lon,sign_type,speed_limit,heading,sub_type
21.036455,105.780037,6,0,,0
20.995,105.732,4,60,180,0
```

- `sign_type`: 1=speed, 2=resident-area, 3=no-overtaking, 4=camera, 5=toll,
  **6=traffic light**, 10=danger/tunnel.
- `speed_limit`: km/h (0 if N/A). `heading`: degrees 0–359 (blank = omni).
- `sub_type`: 0=start/active, 1=end (for resident-area / no-overtaking).

---

## 4. Deployment on `homebridge` (Phase 1 → 4)

**Phase 1 — system packages & code**
```bash
ssh admin@100.107.34.92        # or 192.168.1.65
sudo apt update
sudo apt install -y python3-venv python3-pip git
sudo mkdir -p /opt/viethud-pipeline && sudo chown admin:admin /opt/viethud-pipeline
# copy this folder's contents to /opt/viethud-pipeline (rsync/scp/git)
```

**Phase 2 — virtualenv & deps**
```bash
cd /opt/viethud-pipeline
python3 -m venv venv
./venv/bin/pip install -U pip
./venv/bin/pip install -r requirements.txt      # osmium, requests
```
> If `pip install osmium` needs to build, install headers first:
> `sudo apt install -y build-essential cmake libboost-dev libexpat1-dev zlib1g-dev libbz2-dev`
> (aarch64 wheels usually exist, so this is rarely needed).

**Phase 3 — config & first run**
```bash
cp .env.example .env && chmod 600 .env && nano .env   # GITHUB_TOKEN, TELEGRAM_*, sounds path
mkdir -p assets/sounds && cp -r /path/to/vi_mp3s/* assets/sounds/   # voice prompts

# First run — start SMALL to validate before the full ~350MB VN build:
#   grab a city extract to sanity-check the whole chain quickly:
./venv/bin/python fetch_osm.py                       # or drop a small .osm.pbf into data/osm/
./venv/bin/python synthesize_and_pack.py --region VN
./venv/bin/python github_and_notify.py               # first push seeds the repo
```
Check `logs/pipeline.log` and the printed summary (segments / signs / traffic
lights / names). Verify the device OTA sees the new `manifest.txt`.

**Phase 4 — systemd timers**
```bash
sudo cp systemd/viethud-*.service systemd/viethud-*.timer /etc/systemd/system/
sudo cp logrotate/viethud-pipeline /etc/logrotate.d/
sudo systemctl daemon-reload
sudo systemctl enable --now viethud-osm-fetch.timer viethud-pipeline.timer
systemctl list-timers 'viethud-*'                    # confirm next run times
# manual trigger any time:
sudo systemctl start viethud-pipeline.service && journalctl -u viethud-pipeline -f
```

Schedule: OSM fetch monthly (01st 13:00), pipeline daily (10:00). The daily run
re-packs and **publishes only when the data changed** (manifest body compared
ignoring the version timestamp), so quiet days cost nothing and don't spam
Telegram/GitHub.

---

## 5. Authentication

- **GitHub:** create a fine-grained PAT with *Contents: Read and write* on
  `911273/VietHUD`, put it in `.env` as `GITHUB_TOKEN`. (Or use an SSH deploy key
  and set `VIETHUD_GIT_REMOTE=git@github.com:911273/VietHUD.git`, leaving the
  token blank.)
- **Telegram:** reuse your existing bot token + chat id in `.env`.

---

## 6. Disk / SD card

The Pi boots from its USB **SSD** (rootfs on `sda`), so `/opt` writes hit the SSD,
not the microSD — the 350 MB PBF and rebuilds don't wear the SD. Logs rotate
weekly (×4, `logrotate/`), and journald is size-capped by default. To be extra
safe you can cap the journal: `sudo journalctl --vacuum-size=200M`.

---

## 7. Legality

Map/road/name/point data: © OpenStreetMap contributors, **ODbL**
(openstreetmap.org/copyright) — the attribution is written into `metadata.bin`.
Custom alerts are the operator's own surveyed data. Nothing here fetches or
redistributes third-party proprietary databases.
