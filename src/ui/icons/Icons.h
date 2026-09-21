#pragma once
#include <lvgl.h>

// Bitmap icons for the 2026-09-14 mockup-driven Dashboard redesign.
//
// Why bitmaps instead of the built-in font: the mockup calls for car/
// warning/sun glyphs, but lv_conf.h only enables Montserrat 14/24/28, and
// the compiled-in font's cmap (checked directly in
// .pio/libdeps/*/lvgl/src/font/lv_font_montserrat_14.c) covers just ASCII
// 32-126 plus ~60 extra symbol codepoints (LV_SYMBOL_* icons like GPS/
// AUDIO/SETTINGS/WARNING live in that extra set and are used directly where
// they fit — see Dashboard.cpp). No emoji, no Vietnamese diacritics.
//
// How these were generated (reproducible, no external asset download):
//   1. Render the emoji glyph with Pillow using Windows' own color emoji
//      font (C:/Windows/Fonts/seguiemj.ttf, embedded_color=True), crop to
//      its bounding box, downsample to the target pixel size.
//   2. car_icon / ego_car_icon / warning_icon (originally
//      U+1F697/U+1F699/U+26A0) keep ONLY the glyph's alpha channel
//      (LV_COLOR_FORMAT_A8) — car_icon and ego_car_icon are meant to be
//      recolored at runtime, so their original color doesn't matter.
//      warning_icon was regenerated as a true-color RGB565A8 image instead
//      (see below) once the A8 version turned out to lose the "!" mark.
//   3. sun_icon (U+2600) and the final warning_icon keep true color
//      (LV_COLOR_FORMAT_RGB565A8, matching LV_COLOR_DEPTH=16).
//   4. Converted PNG -> LVGL C array with the LVGL-bundled
//      lvgl/scripts/LVGLImage.py (--ofmt C --cf A8|RGB565A8).
// Regenerate by repeating that recipe if a different size/glyph is needed.
//
// Recolor rule (spec section 14.2: color is the ONLY risk channel, never
// purely decorative — same rule the old rectangle-silhouette targets
// followed):
//   - car_icon: A8, recolored per-target to riskColor()/relation color in
//     refreshDashboard(), exactly like the rectangles it replaces.
//   - ego_car_icon: A8, recolored to the fixed cyan ACCENT_COLOR — "this is
//     you", not a risk signal, same reasoning as the GPS/Radar telltales.
//   - warning_icon / sun_icon / moon_icon: RGB565A8, drawn at their true
//     fixed color, never recolored — decorative chrome only. The actual
//     risk color rides on the text next to them (riskLabel()/riskColor())
//     and on the per-target car icons, so nothing loses its only
//     color-coded signal.
//
// moon_icon (U+1F319, added 2026-09-15) pairs with sun_icon: the clock
// icon swaps between them based on gnss.daytime (see gnss/GNSS.cpp for the
// sunrise/sunset calculation, and Dashboard.cpp's refreshDashboard() for
// the lv_image_set_src() swap) — same generation recipe, same 22x22 size.
extern const lv_image_dsc_t car_icon;       // 40x33  A8       — per-target vehicle silhouette (front view, user-supplied reference 2026-09-15)
extern const lv_image_dsc_t ego_car_icon;   // 40x26  A8       — ego vehicle silhouette (distinct shape, currently unused — see Dashboard.cpp git history for "My Car" icon removal)
extern const lv_image_dsc_t warning_icon;   // 30x30  RGB565A8 — fixed amber/black alert triangle
extern const lv_image_dsc_t sun_icon;       // 22x22  RGB565A8 — clock icon, daytime
extern const lv_image_dsc_t moon_icon;      // 22x22  RGB565A8 — clock icon, nighttime
