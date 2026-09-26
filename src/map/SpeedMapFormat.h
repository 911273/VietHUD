#pragma once
#include <stdint.h>

// Binary speed-map database format — the wire contract between
// tools/map_builder/build_speedmap.py (produces it on a PC from OSM data)
// and SdCardManager.cpp (reads it on the ESP32). Neither side ever touches
// raw OSM data at the same time as the other: the PC tool converts once,
// offline; the ESP32 only ever opens these fixed-layout binary files. See
// SpeedLimitManager.h for the matching algorithm that consumes this data,
// and C:\Users\phamq\.claude\plans\wobbly-swinging-chipmunk.md for the full
// spec this implements (2026-09-15).
//
// Every struct below is `#pragma pack(push, 1)` (no compiler padding) and
// every multi-byte field is little-endian, matching x86/ESP32 native byte
// order on both ends — build_speedmap.py packs with Python's `struct`
// module using a leading `<` (explicit little-endian, no alignment padding)
// so the two sides agree byte-for-byte without either doing endian
// conversion. Latitude/longitude are always int32 degrees x 1e7 (not
// double) — enough precision (~1.1cm) for road-level matching at a quarter
// of the storage cost, per the spec's own suggestion.
//
// On-disk layout, root of the microSD card (format V2, 2026-09-16 — see
// TileIndexEntry's own comment for why V1's one-file-per-tile layout was
// replaced):
//   /speedmap/metadata.bin — one SpeedMapMetadata
//   /speedmap/index.bin    — TileIndexEntry[metadata.tileCount]
//   /speedmap/tiles.bin    — every tile's RoadSegment[] concatenated back to
//                            back; each TileIndexEntry's fileOffset/fileSize
//                            locates its own slice
//   /speedmap/cameras.bin  — CameraPoint[] concatenated back to back, no
//                            index/header of its own (added 2026-09-21 —
//                            see CameraPoint's own comment). OPTIONAL: a
//                            card built before this existed simply doesn't
//                            have this file, and SdCardManager.cpp treats
//                            that as "zero cameras" rather than a load
//                            error — the whole speed-limit map already
//                            works without it, camera warning is a strict
//                            addition, not a new requirement.

#define SPEEDMAP_MAGIC "SLMP" // compared as raw bytes, not as an integer — sidesteps any endianness ambiguity in how "the magic number" gets interpreted
// V2 (2026-09-16): one packed /speedmap/tiles.bin blob instead of one file
// per tile — see TileIndexEntry's own comment for why. Bumped so old V1
// firmware refuses a V2 card loudly (MAP FORMAT ERROR) instead of trying to
// open per-tile files that no longer exist, and vice versa.
#define SPEEDMAP_FORMAT_VERSION 2

#pragma pack(push, 1)

// metadata.bin — one instance, read first and validated before anything
// else is trusted (SdCardManager.cpp). testLatE7/testLonE7/testHeadingDeg/
// testRoadId are NOT part of the spec's own section 34 list — added so the
// ESP32 can self-test its own matching pipeline against a known-good point
// in whatever database is loaded, without needing a live GNSS fix (this
// project has not seen one yet in any session — see the plan's Context).
// testRoadId == 0 means "no self-test point provided", not "road id 0".
struct SpeedMapMetadata {
    char magic[4];          // must be exactly {'S','L','M','P'}
    uint16_t formatVersion; // must match SPEEDMAP_FORMAT_VERSION or the firmware refuses to load it (MAP FORMAT ERROR)
    char region[16];        // e.g. "VN" — human-readable, NOT parsed for logic, null-padded
    char mapVersion[16];    // e.g. "2026.09" — null-padded
    uint64_t buildTimestamp; // unix seconds, informational
    uint32_t tileCount;
    uint32_t segmentCount;   // total across all tiles, informational
    float tileSizeDeg;       // grid cell size in degrees (both axes) — read from here, never hardcoded on the ESP32 side
    int32_t testLatE7;
    int32_t testLonE7;
    int16_t testHeadingDeg;  // degrees, 0-359; only meaningful if testRoadId != 0
    uint32_t testRoadId;     // expected RoadSegment::id at the test point — 0 = no self-test point
    char attribution[64];    // e.g. "(c) OpenStreetMap contributors, ODbL" (spec section 37) — stored for Settings/About use, not shown on the Dashboard
    uint32_t crc32;          // CRC32 of index.bin's raw bytes (not the tiles — see SdCardManager.cpp's mount() comment for why that's enough for V1)
};

// index.bin — TileIndexEntry[metadata.tileCount], sorted by tileId ascending
// (SdCardManager.cpp's neighbor lookup binary-searches this). fileOffset/
// fileSize describe this tile's slice of the single /speedmap/tiles.bin
// blob (format V2, 2026-09-16) — every tile used to be its own file
// (tile_NNNNNN.bin, fileOffset unused) until a real Northern-Vietnam extract
// (62081 tiles) hit a hard SD-card wall: at that card's 16KB FAT cluster
// size, 62081 mostly-~1KB files would have needed ~970MB of slack space
// alone (each file rounds up to a full cluster) — far more than the card's
// ~480MB capacity, even though the actual tile data totalled only ~74MB.
// One packed blob has no such per-file rounding waste. This is exactly the
// V2 the original V1 comment here already anticipated ("kept in the format
// ... so a future V2 could pack tiles into one file without an index format
// change") — the struct layout didn't need to change, only how it's used.
struct TileIndexEntry {
    uint32_t tileId;
    int32_t minLatE7;
    int32_t maxLatE7;
    int32_t minLonE7;
    int32_t maxLonE7;
    uint32_t fileOffset; // unused in V1 — see above
    uint32_t fileSize;   // expected byte size of tile_NNNNNN.bin, checked against the actual file as a cheap corruption guard
};

enum RoadDirection : uint8_t {
    DIR_BIDIRECTIONAL = 0, // this segment's speedLimitKmh applies traveling either way
    DIR_FORWARD = 1,       // applies only traveling start->end (heading ~= headingDeg)
    DIR_BACKWARD = 2,      // applies only traveling end->start (heading ~= headingDeg+180)
    DIR_UNKNOWN = 3,
};

// bit flags for RoadSegment::flags
#define SEGFLAG_HAS_CONDITIONAL 0x01 // OSM maxspeed:conditional existed but isn't evaluated in V1 (spec section 12) — kept so it isn't silently lost
// Grade-separation flags (2026-09-26, elevated-road matching — map/TrackContinuity.h).
// Set by the data builders from OSM tags; absent (0) on older cards, where the
// matcher falls back to topology-only continuity.
#define SEGFLAG_BRIDGE 0x04 // on a bridge/viaduct/flyover (OSM bridge=* other than "no", or layer >= 1)
#define SEGFLAG_TUNNEL 0x08 // in a tunnel/underpass (OSM tunnel=* other than "no", or layer <= -1)
#define SEGFLAG_LINK 0x10   // a ramp/slip road (OSM highway=*_link) — how traffic changes level
#define SEGFLAG_ONEWAY 0x02          // a real legal one-way restriction (OSM oneway=yes), not just "this record only carries one direction's speed" — see SpeedLimitManager.cpp's matcher for why this is distinct from `direction`

enum SpeedSource : uint8_t {
    SPEED_SOURCE_UNKNOWN = 0,
    SPEED_SOURCE_OSM_MAXSPEED = 1,
    SPEED_SOURCE_OSM_FORWARD = 2,
    SPEED_SOURCE_OSM_BACKWARD = 3,
    SPEED_SOURCE_DEFAULT = 4,
    SPEED_SOURCE_CONDITIONAL = 5,
};

// One instance per road segment (a single straight sub-section of an OSM
// way, start/end = two consecutive OSM nodes — build_speedmap.py splits
// each way into one RoadSegment per node pair, not one per whole way, so
// heading/matching stays accurate on curved roads). A two-way street whose
// OSM tags give different maxspeed:forward/maxspeed:backward values becomes
// TWO RoadSegments sharing the same start/end geometry, one DIR_FORWARD and
// one DIR_BACKWARD (see spec section 13's worked example) — the matcher
// picks whichever this fix's heading actually agrees with.
struct RoadSegment {
    uint32_t id;
    int32_t startLatE7;
    int32_t startLonE7;
    int32_t endLatE7;
    int32_t endLonE7;
    uint16_t headingDeg;   // bearing from start to end, 0=North/clockwise, degrees 0-359
    // highway=* classification (real values as of 2026-09-22, populated by
    // tools/map_builder/build_speedmap.py's ROAD_CLASS_BY_HIGHWAY table —
    // was written as a placeholder 0 for every segment before that; see its
    // own comment for the exact highway=* mapping). Consumed by
    // map/MapRenderer.cpp for the live background map's per-class line
    // style/color. 0=unclassified/other, 1=highway/major (motorway/trunk/
    // primary), 2=main road (secondary/tertiary), 3=small/alley
    // (residential/service/track/...).
    uint8_t roadClass;
    uint8_t direction;     // RoadDirection
    int16_t speedLimitKmh; // -1 = UNKNOWN (no maxspeed tag and no applicable default)
    uint8_t speedSource;   // SpeedSource
    uint8_t flags;         // SEGFLAG_*
};

// cameras.bin — a flat CameraPoint[], no tiling/index (added 2026-09-21,
// user-requested "tai du lieu ve canh bao giao thong, gom camera cung nhu
// gioi han toc do"). Sourced from OSM's own highway=speed_camera node tag
// (see build_speedmap.py's extract_cameras()) — same ODbL-licensed data
// this project already ingests and attributes for the speed-limit map, NOT
// Waze: Waze has no public developer API, and its one real data-sharing
// program (Connected Citizens Program) is restricted to government/city
// partners with no commercial or hobbyist use, so it was never a legally
// viable source for this project. Deliberately NOT tiled/indexed like
// RoadSegment above — real speed-camera counts even for a whole country
// are low thousands at most (vs. millions of road segments), small enough
// to load whole into RAM at boot rather than needing tiles.bin's
// lazy-tile-cache treatment.
struct CameraPoint {
    // uint64_t, NOT uint32_t like RoadSegment::id above — that field is a
    // SYNTHETIC counter build_speedmap.py assigns itself (never overflows
    // by construction), but this one is deliberately the RAW OSM node id
    // (so a re-export of the same region is diffable/traceable back to
    // source) and modern OSM node ids have already grown past 2^32 (a real
    // Vietnam extract's speed_camera nodes included id 12531966523) —
    // confirmed the hard way: build_speedmap.py's first version of this
    // used uint32_t and threw `struct.error` on real data the very first
    // time it ran against more than a handful of cameras.
    uint64_t id;
    int32_t latE7;
    int32_t lonE7;
    int16_t speedLimitKmh;  // the camera's own OWN enforced limit if OSM tagged it directly (rare) — -1 = UNKNOWN, fall back to the matched road segment's own speedLimitKmh instead
    uint16_t directionDeg;  // bearing the camera faces/enforces, 0-359 (OSM direction=*) — 0xFFFF = unknown/not tagged (assume it watches BOTH directions of travel, the conservative choice: an unknown-facing camera should still warn rather than silently not warning)
};

// Traffic signs database format (VNSG) — for /speedmap/signs.bin
enum TrafficSignType : uint8_t {
    SIGN_TYPE_UNKNOWN       = 0,
    SIGN_TYPE_SPEED_LIMIT   = 1, // P.127: Speed limit
    SIGN_TYPE_RESIDENT_AREA = 2, // R.420 / R.421: Khu đông dân cư
    SIGN_TYPE_NO_OVERTAKING = 3, // P.125 / DP.133: Cấm vượt
    SIGN_TYPE_CAMERA        = 4, // Camera phạt nguội
    SIGN_TYPE_TOLL_BOOTH    = 5, // P.135: Trạm thu phí
    SIGN_TYPE_TRAFFIC_LIGHT = 6, // Đèn tín hiệu giao thông / camera ngã tư
    SIGN_TYPE_DANGER_OTHER  = 10,// Đoạn đường nguy hiểm / đường hầm / trạm dừng
};

#define SIGN_MAGIC "VNSG"

struct TrafficSignHeader {
    char magic[4];          // "VNSG"
    uint16_t version;       // 1
    uint32_t signCount;     // Total signs in file
    uint16_t reserved;
    uint32_t crc32;
    uint32_t timestamp;
};

struct TrafficSignPoint {
    uint32_t id;
    int32_t latE7;
    int32_t lonE7;
    uint16_t directionDeg;  // 0-359 bearing facing the road, 0xFFFF=omnidirectional
    uint8_t signType;       // TrafficSignType
    uint8_t speedLimitKmh;  // 0 = none / end of restriction, or 20..120
    uint8_t subType;        // 0 = start/active, 1 = end
    uint8_t flags;
    uint16_t reserved;      // aligns to 20 bytes
};

// Road Names Database Format (VNNM) — for /speedmap/names.bin & seg_names.bin
#define NAMES_MAGIC "VNNM"
#define NAMES_VERSION_V1 1
#define NAMES_VERSION_V2 2

struct RoadNamesHeaderV1 {
    char     magic[4];       // "VNNM"
    uint16_t version;        // 1
    uint16_t nameCount;      // Total names (capped at 65,535)
    uint32_t poolBytes;      // Size of UTF-8 NUL-terminated string pool in bytes
    uint32_t reserved;       // Reserved (0)
};

// Format V2 (2026): Lifts 65k limit for nationwide full Vietnam street network
struct RoadNamesHeaderV2 {
    char     magic[4];       // "VNNM"
    uint16_t version;        // 2
    uint32_t nameCount;      // Total names (uint32)
    uint32_t poolBytes;      // Size of UTF-8 NUL-terminated string pool in bytes
    uint16_t reserved;       // Reserved / padding (0)
};
// Note: seg_names.bin is a headerless array indexed by segId (1-based, index 0 unused):
//   - On V1: uint16_t per segment (2 bytes each)
//   - On V2: uint32_t per segment (4 bytes each)
// Value is nameId (0 = unnamed road segment).

#pragma pack(pop)

// Node-coincidence tolerance for chaining RoadSegments into a route
// (map/RoutePredictor.cpp + map/SpeedLimitManager.cpp's routeSegmentProvider).
// Two segment endpoints that represent the SAME junction node must compare equal
// so the forward route can chain through them.
//
// WHERE NODE ALIGNMENT MUST BE SOLVED (2026-09-25): on the PC COMPILER, not here.
// When the map is built from a tile-clipped vector source (e.g. MBTiles cuts
// geometry at tile borders), the two sides of a border node can land on slightly
// different coordinates. The correct, deterministic fix is to NORMALIZE/STITCH
// nodes at compile time — snap every endpoint to a single canonical E7 coordinate
// per real node (a node table keyed by rounded lat/lon), so all segments meeting
// there emit byte-identical start/end coords. The firmware then only needs an
// exact (or near-exact) match. THIS IS A CONTRACT the PC compiler must uphold.
//
// A first version used a 2 m box here as a stopgap, but a ~2 m tolerance (≈2.7 m
// on the diagonal) is NOT a mathematical guarantee against merging two genuinely
// distinct urban nodes — frontage/parallel/service roads, slip lanes and very
// close junctions in a dense city can sit that close. So this is now a TIGHT
// safety margin: ~0.5 m, only to absorb ±1-E7 integer-rounding of an
// already-normalized coordinate — far too small to join distinct road nodes
// (which are ≥1 m apart even in the densest areas). Once the PC compiler's node
// normalization is confirmed, this may be set to 0 (exact match).
// 1 E7 latitude unit ≈ 1.11 cm, so 50 E7 ≈ 0.55 m; at VN latitudes 1 E7 lon unit
// ≈ 1.05 cm, so the lon bound is ≈ 0.52 m.
#define SEG_NODE_MATCH_EPS_E7 50
static inline bool segNodesCoincide(int32_t latA, int32_t lonA, int32_t latB, int32_t lonB) {
    int32_t dLat = latA > latB ? latA - latB : latB - latA;
    int32_t dLon = lonA > lonB ? lonA - lonB : lonB - lonA;
    return dLat <= SEG_NODE_MATCH_EPS_E7 && dLon <= SEG_NODE_MATCH_EPS_E7;
}

// Tile grid: lat/lon are bucketed into tileSizeDeg x tileSizeDeg cells, ID
// = a Cantor-pairing-style encode of the two cell indices so it fits one
// uint32_t and is trivially reversible for logging. Shared math (both this
// header, compiled into the firmware, AND re-implemented as an equivalent
// Python function in build_speedmap.py — see that file's tile_id() doc
// comment for why it's re-implemented instead of shared source).
inline uint32_t speedmapTileId(float latDeg, float lonDeg, float tileSizeDeg) {
    // Offset lon by 180 and lat by 90 first so both cell indices are always
    // >= 0 (no negative-number tile IDs to reason about at the boundaries).
    int32_t latCell = (int32_t)((latDeg + 90.0f) / tileSizeDeg);
    int32_t lonCell = (int32_t)((lonDeg + 180.0f) / tileSizeDeg);
    // OR, not add/XOR: latCell occupies bits 16-31, lonCell bits 0-15, and
    // lonCell never exceeds 16 bits (tileSizeDeg=0.01 -> max ~36000 lon
    // cells, fits with headroom) so the two halves never overlap — a plain
    // OR keeps both cell indices trivially recoverable for logging
    // (latCell = id >> 16, lonCell = id & 0xFFFF).
    return ((uint32_t)latCell << 16) | ((uint32_t)lonCell & 0xFFFF);
}
