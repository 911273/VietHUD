#pragma once

// Legally-required minimum following distance table (feature-requested
// 2026-09-21, "HIEN THI KHOANG CACH AN TOAN THEO TOC DO"), Vietnam —
// Thông tư 38/2024/TT-BGTVT quy định về tốc độ và khoảng cách an toàn của
// xe cơ giới tham gia giao thông đường bộ. A single, real config table
// (not scattered magic numbers in the UI or Risk Engine — that was
// explicitly asked for): ui/Dashboard.cpp is a pure CONSUMER of
// legalMinFollowDistanceM() below, never re-encodes any of these numbers
// itself. Updating the regulation later means editing this one array, not
// hunting through firmware.
//
// Deliberately NOT used to replace or feed radar/LD2451.cpp's real TTC
// engine (radar.audioAllowed, the collision-risk flash overlay) — this is
// a separate, purely informational "does the CURRENT gap meet the legal
// minimum for this speed" display, independent of "are we about to
// collide" (TTC). Conflating the two was explicitly ruled out by the
// feature request ("Khong dung khoang cach phap ly de thay the TTC").
struct SafeDistanceRule {
    float minSpeedKmh, maxSpeedKmh, minDistanceM;
    bool valid;
};

// Matches the feature request's own example table exactly, including the
// deliberate overlap at speed==60 (a real quirk of the regulation itself:
// exactly 60 km/h gets its own lower 35m figure, distinct from the 55m
// that applies just above 60) — legalMinFollowDistanceM() below resolves
// that overlap correctly (see its own comment), rather than needing the
// data reshaped to avoid it.
static const SafeDistanceRule SAFE_DISTANCE_RULES[] = {
    {0, 60, 0, false},     // below 60 km/h: the regulation leaves this to the driver's own judgement, no fixed number
    {60, 60, 35, true},    // exactly 60 km/h
    {60, 80, 55, true},    // above 60, up to and including 80
    {80, 100, 70, true},   // above 80, up to and including 100
    {100, 120, 100, true}, // above 100, up to and including 120
};
static const int kSafeDistanceRuleCount = sizeof(SAFE_DISTANCE_RULES) / sizeof(SAFE_DISTANCE_RULES[0]);

// Returns the legally-required minimum following distance for speedKmh, or
// -1 ("UNKNOWN" — no legal figure applies) below 60 km/h, above 120 km/h,
// or anywhere else no `valid` rule in the table above actually matches.
// Never invents/extrapolates a number outside what the table defines —
// exactly the feature request's own explicit rule for both the <60 and
// >120 cases.
//
// Only ever checks `valid` rules, skipping the `false` ones outright: a
// naive "first range in array order that contains speedKmh" scan would
// incorrectly match the {0,60,...,false} row for speedKmh==60 (0<=60<=60
// is true) before ever reaching the real {60,60,35,true} row that
// immediately follows it in the table — the two DELIBERATELY overlap at
// exactly 60. Skipping straight to valid rules avoids that trap entirely:
// only a genuine match against a real distance value is ever considered.
//
// Each valid rule's own range is treated as (minSpeedKmh, maxSpeedKmh] —
// exclusive lower bound, inclusive upper bound — EXCEPT a singleton rule
// (minSpeedKmh == maxSpeedKmh, i.e. the exactly-60 row), which matches
// only that one exact value. This is what correctly resolves speedKmh==60
// to 35m (the singleton) rather than 55m (the 60-80 bracket, which this
// exclusive-lower-bound convention correctly excludes 60 itself from), and
// resolves speedKmh==100 to 70m (the 80-100 bracket's inclusive upper
// bound) rather than 100m (the 100-120 bracket, whose exclusive lower
// bound correctly excludes exactly 100).
inline float legalMinFollowDistanceM(float speedKmh) {
    for (int i = 0; i < kSafeDistanceRuleCount; i++) {
        const SafeDistanceRule &r = SAFE_DISTANCE_RULES[i];
        if (!r.valid) continue;
        bool isSingleton = r.minSpeedKmh == r.maxSpeedKmh;
        bool matches = isSingleton ? (speedKmh == r.minSpeedKmh) : (speedKmh > r.minSpeedKmh && speedKmh <= r.maxSpeedKmh);
        if (matches) return r.minDistanceM;
    }
    return -1;
}
