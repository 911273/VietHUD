#pragma once
#include <lvgl.h>

// Bitmap icons for the Dashboard.
//
// Why bitmaps instead of the built-in font: warning/sun/moon aren't
// expressible with the compiled-in LVGL font (Montserrat 14/24/28/32/36/48,
// see lv_conf.h) — its cmap (checked directly in
// .pio/libdeps/*/lvgl/src/font/lv_font_montserrat_14.c) covers just ASCII
// 32-126 plus ~60 extra symbol codepoints (LV_SYMBOL_* icons like GPS/
// AUDIO/SETTINGS live in that extra set and are used directly where they fit
// — see Dashboard.cpp). No emoji, no Vietnamese diacritics.
//
// How these were generated (reproducible, no external asset download):
//   1. Render the emoji glyph with Pillow using Windows' own color emoji
//      font (C:/Windows/Fonts/seguiemj.ttf, embedded_color=True), crop to
//      its bounding box, downsample to the target pixel size.
//   2. warning_icon (originally U+26A0) is a true-color RGB565A8 image (an
//      A8-only version lost the "!" mark, see git history).
//   3. sun_icon (U+2600) and moon_icon (U+1F319) keep true color
//      (LV_COLOR_FORMAT_RGB565A8, matching LV_COLOR_DEPTH=16).
//   4. Converted PNG -> LVGL C array with the LVGL-bundled
//      lvgl/scripts/LVGLImage.py (--ofmt C --cf A8|RGB565A8).
// Regenerate by repeating that recipe if a different size/glyph is needed.
//
// car_icon / ego_car_icon (per-target and ego-vehicle silhouettes for the
// radar road-panel view) were removed 2026-09-21 alongside radar itself —
// see git history if that rendering style is ever wanted again for
// something else. warning_icon/sun_icon/moon_icon are drawn at their true
// fixed color, never recolored — decorative chrome only; risk/status color
// lives on the text next to them (Dashboard.cpp), same "color is the only
// risk channel" rule the removed per-target icons used to follow too.
extern const lv_image_dsc_t warning_icon;   // 30x30  RGB565A8 — fixed amber/black alert triangle
extern const lv_image_dsc_t sun_icon;       // 22x22  RGB565A8 — clock icon, daytime
extern const lv_image_dsc_t moon_icon;      // 22x22  RGB565A8 — clock icon, nighttime
