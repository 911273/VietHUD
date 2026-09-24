#pragma once
#include "core/SharedState.h"

// Live background map — heading-up rotated road segments, breadcrumb trail
// and upcoming sign/camera markers, published as screen-space geometry for
// ui/Dashboard.cpp to draw into an lv_canvas (user-requested 2026-09-22,
// "tich hop ban do vector offline lam nen"). Full architecture/reality-check
// writeup: C:\Users\phamq\.claude\plans\proud-riding-flute.md.
//
// Division of labor, deliberately: this module does ALL the geography (flat-
// earth projection, heading-up rotation, zoom scale) on Core 0, publishing
// already-screen-space coordinates via core/SharedState.h's
// MapViewSnapshot; ui/Dashboard.cpp's Core-1 UI task only ever draws lines
// it's handed, never touches lat/lon or trig. Segments/markers themselves
// come from map/SpeedLimitManager.cpp's own already-warm tile cache and
// flat camera/sign arrays (speedLimitManagerGetNearbySegments()/
// GetNearbyMarkers()) rather than this module opening a second path into
// map/SdCardManager.h.

// Screen-space marker kind, decoupled from SpeedMapFormat.h's TrafficSignType
// (which map/SpeedLimitManager.cpp's NearbyMarkerRaw::kind actually carries)
// so core/SharedState.h's MapMarker doesn't need to depend on that header —
// this module is the one place that translates between the two.
enum MapMarkerKind : uint8_t {
    MAP_MARKER_CAMERA = 0,
    MAP_MARKER_RESIDENT_AREA = 1,
    MAP_MARKER_NO_OVERTAKING = 2,
    MAP_MARKER_TRAFFIC_LIGHT = 3,
    MAP_MARKER_TOLL_BOOTH = 4,
    MAP_MARKER_OTHER = 5,
};

// Called once at boot (ui/Dashboard.cpp's buildMapCanvas(), before
// map/SpeedLimitManager.cpp's task starts — see main_viethud.cpp's setup()
// ordering, so there's no cross-task race to guard here) with the map
// canvas's actual pixel size for whichever orientation booted. Every
// transform below derives its ego anchor (screen center horizontally, 2/3
// of the way down vertically — spec: "vị trí xe đặt ở 1/3 phía dưới màn
// hình") and pixel-per-meter scale from this, so this module itself never
// needs to know about screenRotation/orientation branching.
void mapRendererSetCanvasSize(int widthPx, int heightPx);

// Explicit geometry for the oversized heading-up canvas (2026-09-24): the map
// canvas is a `canvasSide`-square buffer with the ego at (anchorX,anchorY) =
// its center, drawn NORTH-UP, then rotated to heading-up by ui/Dashboard.cpp.
// `refMinDim` is the on-screen min(anchorX, anchorY, screenH-anchorY) that
// frames the zoom to the VISIBLE screen independently of the oversized canvas.
// Dashboard calls this instead of mapRendererSetCanvasSize() so the enlarged
// rotation canvas doesn't change the map's zoom.
void mapRendererSetGeometry(int canvasSide, int anchorX, int anchorY, int refMinDim);

// Zoom radius for a given speed — 300m under 40km/h, 600m 40-80, 1000m
// above 80 (spec's own tiers). Exposed so demo/DemoMode.cpp reports the same
// number this module actually uses rather than a second hardcoded copy.
float mapRendererZoomRadiusM(float speedKmh);

// Manual digital-zoom multiplier (the on-screen Zoom button, ui/Dashboard.cpp).
// Folded into mapRendererZoomRadiusM (radius = base / multiplier) so the SAME
// zoom drives both the vector layer AND the raster background (Dashboard reads
// the published zoomRadiusM to scale the raster), keeping them matched. >1 =
// more zoomed in. Default 1.
void mapRendererSetZoomMultiplier(float mult);

// Real path — called once per map/SpeedLimitManager.cpp's existing ~500ms
// task-loop iteration (Core 0). Internally throttled: only recomputes and
// publishes a new MapViewSnapshot when the car has moved >=5m or turned
// >=5 degrees since the last publish (or on the very first call) — the
// user-approved "throttle by real movement, not a fixed Hz" decision (see
// the plan). A car sitting still or driving dead straight costs nothing
// here, and ui/Dashboard.cpp's own redraw check (MapViewSnapshot::
// generation) means a skipped update here also skips the ~33ms full-panel
// SPI flush that a redraw would otherwise force.
void mapRendererUpdate(const GnssSnapshot &gnss);

// Pure transform, no SD access, no throttle gate — given an
// already-supplied segment list, computes and publishes a MapViewSnapshot
// exactly as mapRendererUpdate() would after its own SD fetch. Used by
// mapRendererUpdate() itself AND by demo/DemoMode.cpp directly, so the demo
// exercises this real rotation/projection code with a small synthetic road
// network instead of fabricating pre-transformed screen coordinates by
// hand — consistent with how DemoMode already drives every other real
// widget-update path with fabricated SENSOR input, never a fabricated
// UI-layer shortcut.
void mapRendererComputeFromSegments(const GnssSnapshot &gnss, const struct RoadSegment *segs, int segCount);

// Clears this module's internal breadcrumb-trail history and throttle-gate
// "last position" state, and publishes a blank/invalid MapViewSnapshot —
// called when demo/DemoMode.cpp turns off, mirroring
// demoModeSetEnabled()'s own RoadInfoSnapshot reset, so leftover fake trail
// points from a demo session never bleed into the next real drive.
void mapRendererReset();
