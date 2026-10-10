# VietHUD platform — one core, many boards

Status: phase 0–1 done 2026-10-10 (3.5" and 2.8" on the shared core). P4 and
Lite 1.54" are the next phases (see *Roadmap*).

## Layout

```
platformio.ini            one env per model; [viethud_core] = the shared source list
include/boards/board.h    picks the board from the env's -DVIETHUD_BOARD_* flag
include/boards/<board>/board_config.h
                          pins, panel geometry, identity (model id, OTA channel,
                          release version) and CAPABILITY flags
src/boards/<board>/board.cpp
                          the board's implementation of src/core/Board.h:
                          early init (buses, codec), amp, panel driver, touch
src/core gnss map net log audio update display touch ui
                          shared product code — no board names in here, only
                          BOARD_* capabilities
apps/lite154/             VietHUD Lite 1.54" (stand-alone, button UI) until it
                          moves onto the core (phase 3)
data/ speedmap*/ tools/   shared data format + builders
firmware/<channel>/       OTA channels, one per model (version.json has "model")
```

Shared code may test a **capability** (`#if BOARD_GNSS_PQTM`,
`BOARD_I2S_MCLK_PIN >= 0`, `TFT_RES_W <= 240`) but never a board name.
`include/pincfg.h` / `include/dispcfg.h` remain as one-line shims to
`boards/board.h` for older includes.

## Models

| env | board | screen | model id / OTA channel | version line |
|---|---|---|---|---|
| `viethud` | JC3248W535 | 3.5" 320x480 AXS15231B QSPI, touch | `viethud35` / `firmware/viethud35` | 3.1.x |
| `viethud28` | LCDwiki ES3C28P | 2.8" 240x320 ILI9341V SPI, FT6336, ES8311 | `viethud28` / `firmware/viethud28` | 3.1.x |
| `apps/lite154` | Waveshare S3 LCD 1.54 | 1.54", buttons, no SD | (Lite, own code) / `firmware/` root | 3.4.x |

The bare `firmware/` root is still VietHUD Lite's: deployed Lite units read
it. It must not be reused for another model until Lite has moved to
`firmware/lite154`.

## Capability flags (board_config.h)

| flag | meaning |
|---|---|
| `BOARD_MODEL`, `BOARD_MODEL_ID`, `BOARD_FW_CHANNEL`, `BOARD_FW_VERSION` | identity, OTA folder, release line |
| `BOARD_PANEL_SPI_HZ` | clock for the panel bus (`GFX_NOT_DEFINED` = driver default) |
| `BOARD_DEFAULT_ROTATION`, `BOARD_LANDSCAPE_ONLY` | which orientations have a layout |
| `BOARD_DARK_BG` | darkest background colour the panel shows as black |
| `BOARD_I2S_MCLK_PIN`, `BOARD_AUDIO_NAME` | codec needs MCLK (>=0) or plain I2S amp (-1) |
| `BOARD_GNSS_PQTM`, `GNSS_BAUD` | GPS module dialect / baud |

## Adding a board

1. `include/boards/<name>/board_config.h` — pins + every flag above.
2. `src/boards/<name>/board.cpp` — implement `src/core/Board.h`.
3. A branch in `include/boards/board.h` for `VIETHUD_BOARD_<NAME>`.
4. An env: `build_src_filter = ${viethud_core.src_filter} +<boards/<name>/*>`
   and `-DVIETHUD_BOARD_<NAME>`.
5. If the screen size has no layout yet, add one in `ui/` keyed off the
   screen size (as `buildDashboardCompact()` is for 320x240).

## Data packs

All map + traffic-alert data uses one on-card format (`speedmap` V2:
metadata/index/tiles/names/seg_names/cameras/signs + signed manifest). Each
source is a pack in its own folder — `/speedmap` (default), `/speedmap_wyn`,
`/speedmap_gofa`, and planned `/speedmap_vietmap` — selected in Settings >
Map. Builders live in `tools/`; the same builders must emit an
**alerts-only pack** (cameras + signs, no road tiles) small enough for
on-chip flash, for boards without an SD slot (Lite).

## Release

```
python tools/release_firmware.py viethud28 3.1.2 --notes "..." [--upload-pi]
```
builds with the version stamped, stages `firmware/<channel>/<ver>/firmware.bin`
and `version.json` (with `model`), optionally copies both to the Pi. Then
commit + push the channel folder — devices away from home update from GitHub.
Before every push: all envs build (`pio run -e viethud -e viethud28 -e jc3248w535`,
`pio run -d apps/lite154`) and the host tests pass (`bash test/host/run_track.sh`).

## Roadmap

0. ✅ Lite code moved out of `src/` into `apps/lite154`; bring-up env fixed;
   3.5" channel moved to `firmware/viethud35`; release script.
1. ✅ Board configs + `src/boards/*` for 3.5" and 2.8"; shared code free of
   board names.
2. P4 (`radar_car_p4`, branch `p4-port`) as `boards/p4` — reconcile its
   diverged core files feature by feature. Needs the P4 hardware to verify.
3. Lite 1.54" on the core: shared GNSS/OTA/Wi-Fi/config, alerts-only pack in
   flash, its button UI as `ui/lite`. Moves Lite's OTA to `firmware/lite154`.
4. CI: build every env + host tests on each push; release from tags.
