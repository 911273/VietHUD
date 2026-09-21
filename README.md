# radar_car

Forward vehicle-distance warning/display prototype — ESP32-S3 (JC3248W535) +
HLK-LD2451 radar + u-blox M10N GNSS. See `docs/radar_car_V1.1_spec.md` for
the full specification and `docs/V1.2_hardening_proposal.md` for the
architecture hardening notes (RTOS task/mutex design, watchdog, power,
security, test strategy) that should land before Phase 6 (safety engine).

## Current status

**Phase 1 — board bring-up: done and verified on real hardware (2026-09-14).**
Radar/GNSS/buzzer are not wired yet. `src/main.cpp` brings up the LCD, touch
and a minimal LVGL screen (diagnostics + touch echo).

Two real driver bugs were found and worked around during bring-up — see
`docs/V1.2_hardening_proposal.md` ("Lỗi phần cứng driver AXS15231B..."):
panel `setRotation()` corrupts the display, and partial-region
`draw16bitRGBBitmap()` writes to the wrong address. Fix: never rotate the raw
panel driver, and always draw through an `Arduino_Canvas` RAM framebuffer,
only ever pushing to the physical panel via one full-frame `flush()`. This is
already wired up in `src/main.cpp` — later phases (Dashboard, Settings UI)
that draw through the same `gfx` object don't need to worry about it, just
don't swap `gfx` back to the raw panel driver to "save a buffer".

## Build & flash

```
python -m platformio run                        # build (default env: jc3248w535)
python -m platformio run -t upload              # build + flash (board on COM3)
python -m platformio device monitor             # serial log, 115200 baud
python -m platformio run -e uidemo -t upload    # UI verification demo (see below)
```

(Or `pio ...` directly if the PlatformIO CLI is on PATH.)

**`platform = espressif32@7.1.3` is pinned on purpose** — do not remove the
version. A 2026-09-14 attempt to add an `env:uidemo3` on the pioarduino
platform (Arduino-ESP32 3.x) silently overwrote the shared local platform
install every other env uses too, since pioarduino names its platform
"espressif32" as well — breaking `env:uidemo`'s build with unrelated compile
errors. If you ever touch platform versions/URLs in this file, re-run
`pio run -e uidemo` afterward to confirm it's still building the working
combo (GFX Library for Arduino@1.5.0 / core 2.0.17). See
`docs/V1.2_hardening_proposal.md` "Thử nghiệm esp_lcd" for the full story.

### UI verification demo (`-e uidemo`)

`src/main_ui_demo.cpp` drives the real Dashboard layout / TTC-risk color /
audio-gate hysteresis / radar-GNSS fail-safe display (spec sections 12-14,
28-29, 39, test cases T08-T14) with **simulated** target and GNSS data —
useful for checking the LVGL UI on real hardware before Phase 2 (real radar
UART) exists.

`src/ui/Dashboard.cpp` was redesigned 2026-09-14 into a 3-column cluster
layout (speed+audio | road+targets | warning+TTC) with generated bitmap
icons for the car/warning/sun glyphs the layout needed — see
`src/ui/icons/Icons.h` for why (the compiled LVGL font has no emoji/
Vietnamese glyphs) and how they were generated (reproducible from Windows'
own color emoji font, no external asset download). Labels stayed
English/ASCII for the same reason. Not yet re-verified on real hardware —
only built clean so far (`pio run -e uidemo`); flash and eyeball it before
trusting the pixel layout.

Found and fixed a real crash here: `lv_label_set_text_fmt()`
with a `%f`/`%.Nf` specifier corrupts subsequent varargs and panics
(`LoadProhibited`) because `lv_conf.h` has `LV_USE_FLOAT 0`, which strips
float support out of LVGL's builtin `vsnprintf`. Always `snprintf()` floats
into a buffer yourself and use `lv_label_set_text()` instead — see
`docs/V1.2_hardening_proposal.md` "Lỗi #3" for the full writeup. This applies
to every future phase that formats a float for the UI (Settings, Diagnostics,
Dashboard).

## Hardware

- Board: JC3248W535 (ESP32-S3-N16R8V — 16 MB flash, 8 MB octal PSRAM)
- Display: AXS15231B QSPI, 320x480 capacitive touch
- Custom PlatformIO board definition: `boards/esp32-s3-n16r8v.json`
  (matches USB VID:PID 303A:1001 of the connected device)

Pin mapping: `include/pincfg.h` / `include/dispcfg.h` — LCD/touch pins are
confirmed against real-world working projects for this exact board (see
`docs/V1.2_hardening_proposal.md`). Radar UART / GNSS UART / buzzer pins are
still the *proposed* mapping from the spec and must be verified with a
multimeter/logic analyzer before wiring real sensors (Phase 2+).

## Project layout

```
radar_car/
├── boards/esp32-s3-n16r8v.json   custom PlatformIO board def
├── docs/                         spec + hardening proposal
├── include/                      lv_conf.h, pincfg.h, dispcfg.h
├── lib/AXS15231B_Touch/          I2C touch driver for the AXS15231B panel
├── src/main.cpp                  Phase 1 demo (LCD + touch + LVGL)
└── platformio.ini
```

Radar/GNSS/tracker/safety/audio/config/ui/web modules described in the spec
(`src/radar/`, `src/gnss/`, `src/tracking/`, ...) will be added phase by
phase per the roadmap in `docs/radar_car_V1.1_spec.md` section 31/80.
