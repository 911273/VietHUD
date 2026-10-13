# VietHUD platform — one firmware, one data set, many boards

Status 2026-10-12: all four models build from this one tree at platform
version **4.0.0** — 3.5" (JC3248W535), 2.8" (ES3C28P), Lite 1.54" (Waveshare
S3 LCD 1.54) and 4.3" (Guition JC4880P443C, ESP32-P4). Only the hardware layer
and the screen layout differ per model; firmware versioning, data and OTA are
the same for all.

## Models

| model id | env | board | screen / input | data |
|---|---|---|---|---|
| `viethud35` | `viethud` | JC3248W535 (ESP32-S3) | 3.5" 320x480 AXS15231B QSPI, touch | microSD |
| `viethud28` | `viethud28` | LCDwiki ES3C28P (ESP32-S3) | 2.8" 320x240 ILI9341V, FT6336 touch | microSD |
| `viethud154` | `viethud_lite154` | Waveshare S3 LCD 1.54 (ESP32-S3) | 1.54" 240x240 ST7789, 3 buttons | on-chip flash (`speedmap_lite154`) |
| `viethud43` | `viethud_p4` | Guition JC4880P443C (ESP32-P4 + C6) | 4.3" 800x480 MIPI-DSI, GT911 touch | microSD, else on-chip flash (read-only) |

Screens: the 3.5", 2.8" and 4.3" run the LVGL dashboard with the live map.
The Lite (`BOARD_UI_LITE`) runs the stand-alone Lite firmware's own HUD —
big limit sign, speed, alert card, road name, battery, button settings menu,
Wi-Fi/OTA page — drawn straight with Arduino_GFX, no LVGL and no map render
(`src/boards/lite154/ui/`). Everything under it (GNSS, map matching, alert
sounds in `core/AlertAudio.cpp`, settings in `AppConfig`, Wi-Fi, OTA) is the
shared core.

`tools/models.py` is the registry every tool reads. `env:viethud_p4_sim` is a
bench build of the P4 that drives a simulated car (never for a car).

## Layout

```
platformio.ini             one env per model; [viethud_core] = the shared source list
include/boards/board.h     picks the board from -DVIETHUD_BOARD_*; capability defaults
include/boards/<board>/board_config.h   pins, panel, identity, CAPABILITY flags
src/boards/<board>/        board.cpp (src/core/Board.h) + any board-only drivers
                           (the P4 brings its own display/touch/audio-out + 1.5x fonts)
src/core gnss map net log audio update display touch ui   shared product code
speedmap/                  THE data set (signed manifest), used by every model
speedmap_<profile>/        cut of speedmap/ for flash-only boards (published, signed)
firmware/release.json      THE firmware manifest: one version, one image per model
firmware/<ver>/<model>.bin the images
firmware/<model>/          pre-4.0 per-model channel (kept for units on 3.x)
firmware/version.json      old stand-alone Lite's channel (do not reuse)
apps/lite154/              old stand-alone Lite firmware (3.5.8), legacy only
hardware/                  enclosures
releases/                  frozen releases + archived trees (gitignored)
```

Shared code tests a **capability** (`#if BOARD_HAS_SD_SLOT`,
`BOARD_I2S_MCLK_PIN >= 0`, `TFT_RES_W <= 240`), never a board name. The one
exception is `VIETHUD_P4`: the chip-family switch for IDF-5-only APIs (MIPI-DSI,
PPA, i2s_std, temperature_sensor, C6 BLE) set by the P4 env.

## Capability flags

| flag | meaning |
|---|---|
| `BOARD_MODEL`, `BOARD_MODEL_ID`, `BOARD_FW_CHANNEL` | identity; the model id selects the OTA image |
| `TFT_RES_W/H` | native panel size in UI units (layout choice) |
| `BOARD_DEFAULT_ROTATION`, `BOARD_LANDSCAPE_ONLY` | orientations with a layout |
| `BOARD_DARK_BG` | darkest background the panel shows as black |
| `BOARD_I2S_MCLK_PIN`, `BOARD_AUDIO_NAME` | codec (MCLK >= 0) or plain I2S amp (-1) |
| `BOARD_HAS_TOUCH` | 0 = physical buttons |
| `BOARD_UI_LITE`, `BOARD_HAS_MAP` | Lite HUD instead of the LVGL dashboard; skip the map render |
| `BOARD_HAS_SD_SLOT`, `BOARD_SD_4BIT`, `BOARD_SD_LDO_CHAN` | card slot wiring |
| `BOARD_HAS_FLASH_DATA`, `BOARD_FLASH_DATA_READONLY` | data in the `ffat` partition |
| `BOARD_DATA_PROFILE` | online data channel folder (default `speedmap`) |
| `BOARD_GNSS_PQTM`, `GNSS_BAUD` | GPS module dialect / baud |

## Firmware: one version, auto-selected per hardware

`src/core/Version.h` holds the one platform version. A release builds every
model at the same commit:

```
python tools/release.py 4.0.1 --notes "..." [--upload-pi]
```

It writes `firmware/<ver>/<model>.bin` and `firmware/release.json`:

```json
{"version": "4.0.1", "notes": "...",
 "images": {"viethud35": {"chip": "esp32s3", "path": "4.0.1/viethud35.bin", "size": ..., "sha256": "..."},
            "viethud43": {"chip": "esp32p4", ...}, ...}}
```

A device fetches `release.json` (Pi LAN → mDNS → Tailscale → GitHub), looks
itself up by its model id and downloads only its own image. Before the new
image can boot, `net/FwUpdater.cpp` checks it against the hardware:

1. chip id in the image header = this chip (an S3 never takes a P4 image);
2. the image's identity marker `VIETHUD-HW-ID:<model>;` = this model;
3. SHA-256 = `release.json`.

Any mismatch aborts; the running firmware stays. `release.py` runs the same
three checks on every image before staging it. It also rewrites
`firmware/<model>/version.json`, so units still on 3.x (which only read that)
get 4.x and from then on use `release.json`.

Build/flash one model: `python tools/build.py <env> [upload|uploadfs] [--port COMx]`
(`all` = every model). It picks the toolchain: S3 = default PlatformIO core
(espressif32@7.1.3); P4 = isolated pioarduino core `~/.platformio_p4`.

## Data

Firmware goes to GitHub + the Pi; **data lives on the Pi only**
(`http://<pi>/viethud/`, no 100 MB GitHub limit). A unit reaches it on the
home Wi-Fi (LAN `192.168.1.65`, then mDNS `homebridge.local`) and away from
home through a phone hotspot whose phone runs Tailscale (`100.107.34.92`) —
`net/DataSources.h` tries the three in that order.

| channel | content | publisher | read by |
|---|---|---|---|
| `speedmap/` | full OSM set: **every** road incl. alleys (footways drawn, never matched), cameras/signs, your surveyed points | SpeedMap Studio → 🚀 Phát hành | `net/DataUpdater` (3.5", 2.8", P4 with a card) |
| `speedmap_lite154/` | main roads of the north (class 1 everywhere, class 2 ~100 km round Ha Noi, 15 m simplified) + the region's alerts + Lite names | same | `net/DataUpdater` (Lite) |
| `roadnames/` | every named road incl. alleys, whole country (OSM) | `tools/roadnames-job` on the Pi, weekly | `net/NamesUpdater` (card boards) |
| `roadnames_lite154/` | same, ~33 km around Ha Noi | same job | `net/NamesUpdater` (Lite) |

Devices check automatically **at most once a week** (`dataUpdateAutoCheckStart`);
a check the user asks for runs at once. Updates download in update mode
(reboot), are SHA-256 checked, and the speed-map set installs atomically with
rollback.

### SpeedMap Studio (`python tools/speedmap_studio/server.py`, http://localhost:8088)

- **🌐 Cập nhật OSM** downloads the latest Geofabrik Vietnam extract
  (`data/osm/`, only when newer) and rebuilds the working set
  `data/studio/speedmap/` with `--roads everything` (~20 min, ~290 MB).
- View / add / move / delete cameras, signs, lights. Every edit is written at
  once to `data/custom_alerts/` (`studio.csv` = your points, `suppress.csv` =
  OSM points you deleted); the next OSM rebuild merges them back in. That
  folder is the only data in git.
- **💾 Lưu** writes `cameras.bin` + `signs.bin` of the working set.
- **🚀 Phát hành** signs and uploads `speedmap/` to the Pi (upload to
  `<channel>.new`, then one swap), builds the Lite flash set
  (`data/studio/flash/`, `--roads major --bbox north --simplify-m 15`), cuts the
  Lite/P4 flash images (`tools/build_flash_data.py all --src ...`), publishes
  `speedmap_lite154/`, and commits + pushes `data/custom_alerts/`.

OSM has very few cameras (≈40 nationwide): camera coverage comes from your
own surveyed points.

### Flash boards

`python tools/build_flash_data.py all --src data/studio/flash/speedmap` cuts the
Lite (11.1 MB, ~2 MB kept free to stage updates) and P4-without-card (9.4 MB,
read-only) images: class-1 roads of the whole north first, then class-2 roads
outward from Ha Noi while they fit; the region's alerts; Ha Noi street names;
`sounds/vi/`. Load over USB (`tools/build.py viethud_lite154 uploadfs`, or the
Update Tool).

### VIETHUD_UPDATE_TOOL (`tools/update_tool`)

Plug a unit in by USB: it identifies the board (chip + `VIETHUD-HW-ID` marker
in the running image), flashes the matching firmware from the Pi/GitHub release
(full flash incl. bootloader + table when the partition table differs), and
updates data — flash boards: image cut from Studio's flash set (or the Pi's
`speedmap_lite154/`) written with uploadfs; card boards: the Pi's `speedmap/`
downloaded (resumable, SHA-checked) and pushed over Wi-Fi. It also publishes
firmware (`release.py --upload-pi`) and opens Studio.

## Old stand-alone Lite (3.x) units

They run `apps/lite154` on the stock `default_16MB` partition table and read
`firmware/version.json`. The core firmware needs `partitions_lite154.csv`
(a FAT data partition), which OTA cannot change — so moving a Lite to the
platform takes **one USB flash** (`build.py viethud_lite154 upload` +
`uploadfs`); after that it updates like every other model. Never publish a
core image into `firmware/version.json`.

## Adding a board

1. `include/boards/<name>/board_config.h` — pins + flags above.
2. `src/boards/<name>/board.cpp` — implement `src/core/Board.h`.
3. A branch in `include/boards/board.h` for `VIETHUD_BOARD_<NAME>`.
4. An env with `build_src_filter = ${viethud_core.src_filter} +<boards/<name>/*>`.
5. An entry in `tools/models.py` (and a data profile if it has no card slot).
6. A layout in `ui/` if the screen class is new.

## Release checklist

All four models build (`python tools/build.py all`), host tests pass
(`bash test/host/run_track.sh`), then `tools/release.py`. Hardware tests
before publishing: at least one S3 model and the P4 on the bench.
