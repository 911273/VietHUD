#pragma once
#include <stdbool.h>

// Scripted UI demo (user-requested 2026-09-22, "demo hien thi truoc de toi
// chinh sua"). Publishes synthetic GnssSnapshot/RoadInfoSnapshot values into
// core/SharedState.h so the REAL Dashboard, alert card and audio pipeline
// render every state they can reach — without a GNSS fix, without driving,
// and without a speed-map database on the card.
//
// That last part is why this exists at all rather than being a nicety: the
// board's own microSD has no /speedmap/ database on it yet (see the README's
// "Current status"), so map/SpeedLimitManager.cpp reports mapLoaded=false and
// the sign/camera/speed-limit half of the UI has nothing to show. There was
// no way to look at the alert card's appearance, its distance-driven
// escalation or its voice cues before this.
//
// Runtime-only, deliberately NOT an AppConfig/NVS field: same reasoning as
// WiFi's on/off state (see net/WebPortal.h) — a diagnostic mode that silently
// survived a reboot and kept feeding the driver fake speed limits while
// actually driving would be genuinely dangerous. Always starts OFF.
void demoModeStart();

// Toggled from Settings > Display. Turning it OFF hands the display straight
// back to the real GNSS/map tasks (they never stopped running — they just
// stop publishing while this is on, see gnss/GNSS.cpp and
// map/SpeedLimitManager.cpp).
void demoModeSetEnabled(bool on);

// Read by gnss/GNSS.cpp and map/SpeedLimitManager.cpp to suppress their own
// publishes while the demo owns the shared state. A plain bool read across
// tasks with no lock — same conscious simplification AppConfig.h documents
// for cfg's own cross-task reads: a single aligned bool has no torn
// intermediate value to race on.
bool demoModeIsEnabled();

// Human-readable name of the scene currently playing, for the Settings row
// that shows what's on screen ("Camera 300->30m", "Qua toc do", ...). Returns
// "OFF" while disabled.
const char *demoModeSceneName();
