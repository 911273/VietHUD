#pragma once
#include <stddef.h>
#include <stdint.h>

// Road-name channel (2026-10-13): roadnames.bin (OpenStreetMap, every street
// incl. alleys — map/RoadNames.h) is published on its OWN signed channel,
// <repo>/<BOARD_NAMES_CHANNEL>/{manifest.txt,manifest.txt.sig}, rebuilt weekly
// on the Pi from OSM only. It is independent of the speed-map/alert channel
// (cfg.dataUpdateUrl): its own version, kept in <data dir>/roadnames.ver, and
// installed by itself (download -> SHA-256 check -> replace), so neither
// channel's publisher can drop or overwrite the other's files.
//
// manifest.txt:  "version YYYY.MM.DD.HHMM\nroadnames.bin <size> <sha256>\n",
// signed with the same key as the data manifest (tools/sign_manifest.py).

// Fetches the remote manifest (small) and compares it with the local version.
// Call from a task with Wi-Fi up. Returns true when a newer one is available.
bool namesCheck();
bool namesUpdateAvailable();
const char *namesRemoteVersion();

// Downloads + installs it (run in update mode — TLS needs the free RAM).
// 1 installed, 2 already current, 0 failed. `progress` gets 0..100.
int namesUpdateRun(uint8_t *buf, size_t bufSize, void (*progress)(int pct, const char *msg));
