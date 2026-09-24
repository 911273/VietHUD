#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

// RasterMapManager - Reads and manages compressed Dark Mode JPEG map tiles
// from /speedmap/maptiles.bin on the microSD card for VietHUD.
// Uses a compact two-level sparse index (~31 KB) instead of loading the
// entire 4MB spatial index into PSRAM, preserving memory for the road network.

#define MAPTILES_MAGIC "VNMB"
#define MAPTILES_VERSION 1

#pragma pack(push, 1)
struct MapTileHeader {
    char magic[4];          // "VNMB"
    uint16_t version;       // 1
    uint8_t minZoom;        // e.g. 9
    uint8_t maxZoom;        // e.g. 15
    uint32_t totalTiles;    // Count of tiles in index
    uint32_t indexOffset;   // Byte offset of spatial index
    uint32_t indexSize;     // Byte length of spatial index
    uint8_t reserved[44];   // Aligns to 64 bytes
};

struct MapTileIndexEntry {
    uint64_t tileKey;       // (z << 40) | (x << 20) | y
    uint32_t fileOffset;    // Byte offset of JPEG payload
    uint16_t dataSize;      // Length of JPEG in bytes (~3.5-4.5 KB)
    uint16_t reserved;      // 16 bytes total
};

struct MapTileSparseEntry {
    uint64_t startKey;      // First tileKey in this block
    uint32_t fileOffset;    // Byte offset in maptiles.bin of this block of entries
    uint16_t entryCount;    // Number of entries in this block (up to kBlockEntries)
};
#pragma pack(pop)

class RasterMapManager {
public:
    static RasterMapManager &instance() {
        static RasterMapManager inst;
        return inst;
    }

    // Mounts and reads maptiles.bin index table into a compact sparse index (only ~31 KB RAM)
    bool begin(const char *path = "/speedmap/maptiles.bin");

    // Dynamic map source switching (0 = CartoDB Dark, 1 = OpenStreetMap OSM, 2 = OSM Voyager)
    bool setMapSource(uint8_t sourceIdx);
    uint8_t getMapSource() const { return currentSourceIdx; }
    const char *getCurrentSourcePath() const { return filePath; }
    const char *getSourceName() const;
    void unload();

    bool isLoaded() const { return loaded; }
    uint32_t getTileCount() const { return header.totalTiles; }
    uint8_t getMinZoom() const { return header.minZoom; }
    uint8_t getMaxZoom() const { return header.maxZoom; }

    // Converts WGS84 coordinates to tile coordinates and pixel sub-offset within that tile (0..255)
    static void latLonToTile(float latDeg, float lonDeg, uint8_t zoom,
                             uint32_t &outX, uint32_t &outY, float &outSubPxX, float &outSubPxY);

    // Retrieves compressed JPEG buffer from SD card / LRU cache in PSRAM
    bool getTileJpeg(uint8_t zoom, uint32_t x, uint32_t y, const uint8_t **outJpg, size_t *outSize);

    // Retrieves persistent lv_image_dsc_t for LVGL draw task
    bool getTileDsc(uint8_t zoom, uint32_t x, uint32_t y, const lv_image_dsc_t **outDsc);

    // Renders background tiles onto an LVGL canvas layer around (centerLatDeg, centerLonDeg)
    void renderBackground(lv_layer_t *layer, float centerLatDeg, float centerLonDeg,
                          uint8_t zoom, int canvasW, int canvasH, int anchorX, int anchorY,
                          float scale = 2.0f);

private:
    RasterMapManager();
    MapTileHeader header;
    bool loaded;
    uint8_t currentSourceIdx;
    char filePath[64];

    // Two-level Sparse Index: ~31 KB total in PSRAM instead of 4 MB
    static const int kBlockEntries = 128;
    MapTileSparseEntry *sparseIndex;
    int numSparseBlocks;

    // 1-block cache for 128 entries (2048 bytes) in RAM
    mutable int cachedBlockIdx;
    mutable MapTileIndexEntry blockBuf[kBlockEntries];

    // LRU cache for 8 JPEG tiles in PSRAM to eliminate SD thrashing
    struct CachedTileSlot {
        uint64_t key;
        uint8_t *buf;
        size_t size;
        uint32_t lastUsedMs;
        lv_image_dsc_t imgDsc;
    };
    static const int kCacheSlots = 20;
    CachedTileSlot cache[kCacheSlots];

    bool findTileEntry(uint64_t key, MapTileIndexEntry &outEntry) const;
};
