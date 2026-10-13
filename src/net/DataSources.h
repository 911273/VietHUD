#pragma once
#include <Arduino.h>

// Data lives on the Pi only (2026-10-13: firmware on GitHub + Pi, data on the
// Pi — no 100 MB GitHub limit). A unit reaches it on the home Wi-Fi by its LAN
// address / mDNS name, and away from home through a phone hotspot whose phone
// runs Tailscale, by the Pi's Tailscale address. A data URL pointing at the Pi
// LAN address is tried on all three; the first that answers is used for the
// whole download. Any other URL (set by hand) is used as is.
#define VH_PI_LAN "http://192.168.1.65/viethud/"
#define VH_PI_MDNS "http://homebridge.local/viethud/"
#define VH_PI_TAILSCALE "http://100.107.34.92/viethud/"

// Fills up to 3 candidate base URLs for `url` (ending in '/'); returns how many.
inline int dataSourceCandidates(const String &url, String out[3]) {
    String u = url;
    if (u.length() && !u.endsWith("/")) u += "/";
    if (!u.startsWith(VH_PI_LAN)) {
        out[0] = u;
        return u.length() ? 1 : 0;
    }
    String rest = u.substring(strlen(VH_PI_LAN));
    out[0] = u;
    out[1] = String(VH_PI_MDNS) + rest;
    out[2] = String(VH_PI_TAILSCALE) + rest;
    return 3;
}
