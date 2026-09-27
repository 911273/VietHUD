#pragma once
#include <stdint.h>
#include "OsmTrafficSignals.h"

// Merges built-in OSM traffic lights into a sign array (pure — no Arduino/SD,
// host-testable: test/host/test_signmerge.cpp). `signs` holds `count` entries
// and has room for `cap`; each OSM light is appended as a traffic light unless
// the array already has a traffic light within `dedupM` metres. Returns the new
// count; *outAdded / *outDupes (optional) report what happened. `scratch` must
// hold 2 * (number of existing lights) int32 (e.g. 2 * count). Generic over the
// in-RAM sign record (SdCardManager.h's SignPt) so this header stays standalone.
template <class SignT>
int mergeOsmLights(SignT *signs, int count, int cap, const OsmSignalPoint *osm, int nOsm, uint8_t lightType,
                   float dedupM, int32_t *scratch, int *outAdded, int *outDupes);

#include "SignMerge.inl"
