#pragma once
#include <stdint.h>

// Road-NAME layer (<data dir>/roadnames.bin, "VHRN", built by
// tools/map_builder/build_road_names.py from OpenStreetMap): every named road
// including alleys (Ngõ / Ngách / Hẻm / Kiệt), which the speed map's road
// segments don't carry. Used for the on-screen street name on every model;
// the speed limit and warnings still come from the matcher.
//
// Same rules as the stand-alone Lite's road names: nearest road within 30 m,
// a small penalty per road class so a main road wins a tie with a parallel
// alley, heading check while moving, a challenger must win ~2 s in a row to
// replace the current name, and the last name is kept 8 s off-road.
// Called from the speed-limit task only (reads go through SdCardManager).

// Returns the current road name (UTF-8) or "" — throttled internally to
// ~700 ms; cheap to call every matcher tick. False/"" when the data set has
// no roadnames.bin.
const char *roadNamesLookup(float lat, float lon, bool headingValid, float headingDeg, float speedKmh);

// Forget the open file (data set swapped / remounted).
void roadNamesReset();
