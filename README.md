# VietHUD (formerly radar_car)

GPS-only offline speed-limit / speed-camera / traffic-sign warning device —
ESP32-S3 (JC3248W535) + u-blox M10N GNSS + an offline speed-map database
built from OpenStreetMap + a WYN traffic-signs/road-network export, loaded
from a microSD card at boot. No radar, no cellular/data connection needed
while driving — everything the device warns about comes from GNSS position
plus data already on the card.

This project started as `radar_car`, a forward-collision radar display
(HLK-LD2451 + GNSS). Radar was removed entirely 2026-09-21 — this is a full
product replacement, not a side-by-side option. `docs/radar_car_V1.1_spec.md`
and `docs/V1.2_hardening_proposal.md` are kept for historical record (board
bring-up notes, driver bugs found/fixed, the original radar architecture)
but no longer describe the current product end to end.

## What it does

- Reads GNSS position/speed/heading from a real u-blox M10N (UART2).
- Matches the current position against an offline speed-map database
  (`data/speedmap/*.bin`, built by `tools/map_builder/build_speedmap.py`
  from OSM + WYN data) to show the current speed limit, an upcoming
  speed-limit change, upcoming speed cameras, and upcoming resident-area /
  no-overtaking / toll-booth / traffic-light signs — see
  `src/map/SpeedMapFormat.h` for the on-disk format and
  `src/map/SpeedLimitManager.cpp` for the matching algorithm.
- Shows all of that on an LVGL Dashboard (`src/ui/Dashboard.cpp`) with a
  sign/camera alert card, plays a tone chime immediately on a new warning
  and queues a Vietnamese voice line for it (`src/audio/AudioPlayer.cpp`,
  MP3s in `data/speedmap/sounds/vi/`).
- Logs GNSS speed + speed-limit/camera/sign state to the microSD card per
  drive (`src/log/TripLogger.cpp`), and serves live telemetry / config / OTA
  update over its own WiFi AP (`src/net/WebPortal.cpp`).

## Current status

Builds clean (`pio run -e viethud`) and has been flashed to real hardware
for a boot smoke test. The offline speed-map database on `data/speedmap/`
(built from a real Northern-Vietnam OSM extract + the WYN CSV/network
export) has NOT yet been physically copied onto the board's own microSD
card — see "microSD card contents" below for what to copy and where. Until
that's done, the device boots and runs fine (`SdCardManager.cpp` treats a
missing/absent database as "zero signs/cameras/segments", not a load error)
but the sign/camera/speed-limit/voice features have no live data to show.

## Build & flash

```
python -m platformio run -e viethud              # build the product firmware
python -m platformio run -e viethud -t upload    # build + flash (board on COM3, 921600 baud)
python -m platformio device monitor -e viethud   # serial log, 115200 baud
```

(Or `pio ...` directly if the PlatformIO CLI is on PATH.) `env:viethud` is
also `default_envs`, so a bare `pio run`/`pio run -t upload` builds it too.

Two other envs exist, both hardware-recovery diagnostics, not the product:
- `env:jc3248w535` — builds `src/main.cpp`, the original Phase-1 bring-up
  stub (LCD + touch + a minimal LVGL screen). Useful for isolating a
  display/touch hardware issue from application logic.
- `env:rawtest` — `src/main_rawtest.cpp`, no LVGL at all, just raw
  `Arduino_GFX` calls. The most minimal possible "is the panel alive" check.

`env:uidemo3` is an abandoned esp_lcd migration experiment (unrelated to
this project's direction) — do not build it; see its own comment in
`platformio.ini` and `docs/V1.2_hardening_proposal.md` "Thử nghiệm esp_lcd".

**`platform = espressif32@7.1.3` is pinned on purpose** — do not remove the
version. Installing `env:uidemo3`'s pioarduino platform silently overwrites
the shared local platform install every other env uses too (both name their
platform "espressif32"), breaking `env:viethud`'s build with unrelated
compile errors. If you ever touch platform versions/URLs in this file,
re-run `pio run -e viethud` afterward to confirm it still builds clean.

Found and fixed a real crash early on: `lv_label_set_text_fmt()` with a
`%f`/`%.Nf` specifier corrupts subsequent varargs and panics
(`LoadProhibited`) because `lv_conf.h` has `LV_USE_FLOAT 0`, which strips
float support out of LVGL's builtin `vsnprintf`. Always `snprintf()` floats
into a buffer yourself and use `lv_label_set_text()` instead — applies
everywhere this UI formats a float (Settings, Dashboard).

## microSD card contents

Copy the whole `data/speedmap/` folder onto the root of the physical
microSD card, so the card ends up with:

```
/speedmap/metadata.bin
/speedmap/index.bin
/speedmap/tiles.bin
/speedmap/cameras.bin
/speedmap/signs.bin
/speedmap/sounds/vi/*.mp3            (top-level voice clips)
/speedmap/sounds/vi/speed/*.mp3      (numbered speed-value voice clips)
/speedmap/sounds/vi/slowdown/voice.mp3
/speedmap/sounds/vi/welcome/voice.mp3
```

The board expects the SD card in its dedicated SD_MMC slot (1-bit mode —
see `include/pincfg.h`'s `SD_MMC_CLK_PIN`/`CMD_PIN`/`D0_PIN` comment for why
it's SD_MMC and not SPI). `metadata.bin`/`index.bin`/`tiles.bin` are
required for the speed-limit map to work at all; `cameras.bin`/`signs.bin`
are optional (an absent/empty file is treated as "zero cameras/signs", not
an error) but needed for camera and sign warnings.

## Rebuilding the speed-map database

```
python tools/map_builder/build_speedmap.py <region>.osm data --region <code> --map-version <YYYY.MM> \
    --wyn-signs <traffic_signs.csv> --wyn-network <wmap_network.db>
```

writes `data/speedmap/{metadata,index,tiles,cameras,signs}.bin`. See
`tools/map_builder/build_speedmap.py`'s own docstring and
`src/map/SpeedMapFormat.h` for the exact wire format each file follows.

## Hardware

- Board: JC3248W535 (ESP32-S3-N16R8V — 16 MB flash, 8 MB octal PSRAM)
- Display: AXS15231B QSPI, 320x480 capacitive touch
- GNSS: u-blox M10N on UART2 — see `include/pincfg.h` for the confirmed
  wiring
- microSD: onboard SD_MMC slot (1-bit mode)
- Audio: onboard NS4168 I2S power amp (BCLK=42, LRCK=2, DOUT=41)
- Custom PlatformIO board definition: `boards/esp32-s3-n16r8v.json`
  (matches USB VID:PID 303A:1001 of the connected device)

## Project layout

```
radar_car/
├── boards/esp32-s3-n16r8v.json   custom PlatformIO board def
├── data/speedmap/                offline speed-map database + voice mp3s (copy to the SD card)
├── docs/                         historical spec + hardening proposal (radar-era, not fully current)
├── include/                      lv_conf.h, pincfg.h, dispcfg.h
├── lib/AXS15231B_Touch/          I2C touch driver for the AXS15231B panel
├── src/
│   ├── main_viethud.cpp          the product entry point (env:viethud)
│   ├── main.cpp                  Phase-1 bring-up stub (env:jc3248w535)
│   ├── main_rawtest.cpp          raw display/touch diagnostic (env:rawtest)
│   ├── core/                     AppConfig, NVS persistence, mutex-protected shared state
│   ├── gnss/                     u-blox M10N UART driver + NMEA parsing
│   ├── map/                      microSD speed-map loader + GNSS map-matching
│   ├── ui/                       Dashboard + Settings (LVGL)
│   ├── audio/                    tone chimes + queued Vietnamese voice playback
│   ├── net/                      WiFi AP + live telemetry/config/OTA web portal
│   ├── log/                      per-drive trip CSV logging
│   ├── display/, touch/          display driver + touch task
│   └── main_ui_demo_esplcd.cpp   abandoned esp_lcd experiment (env:uidemo3, do not build)
├── tools/map_builder/            PC-side OSM+WYN -> speed-map database builder
└── platformio.ini
```
