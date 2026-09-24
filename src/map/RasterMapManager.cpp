#include "RasterMapManager.h"
#include <esp_task_wdt.h>
#include "SdCardManager.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// LRU tile buffer size. Raised from 8192 to 12288 on 2026-09-24: a quality
// scan of maptiles.bin found 223 tiles between 8192 and ~10666 bytes that the
// old 8192 cap rejected (rendered blank). 12288 covers them with headroom.
static const size_t kTileBufBytes = 12288;

RasterMapManager::RasterMapManager()
    : sparseIndex(nullptr), numSparseBlocks(0), cachedBlockIdx(-1), loaded(false), currentSourceIdx(0) {
    memset(&header, 0, sizeof(header));
    memset(filePath, 0, sizeof(filePath));
    memset(blockBuf, 0, sizeof(blockBuf));
    for (int i = 0; i < kCacheSlots; i++) {
        cache[i].key = 0;
        cache[i].buf = nullptr;
        cache[i].size = 0;
        cache[i].lastUsedMs = 0;
        memset(&cache[i].imgDsc, 0, sizeof(lv_image_dsc_t));
    }
}

void RasterMapManager::unload() {
    if (sparseIndex) {
        heap_caps_free(sparseIndex);
        sparseIndex = nullptr;
    }
    for (int i = 0; i < kCacheSlots; i++) {
        if (cache[i].buf) {
            heap_caps_free(cache[i].buf);
            cache[i].buf = nullptr;
        }
        cache[i].size = 0;
        cache[i].key = 0;
        cache[i].lastUsedMs = 0;
        memset(&cache[i].imgDsc, 0, sizeof(lv_image_dsc_t));
    }
    numSparseBlocks = 0;
    cachedBlockIdx = -1;
    memset(&header, 0, sizeof(header));
    loaded = false;
}

bool RasterMapManager::setMapSource(uint8_t sourceIdx) {
    unload();
    currentSourceIdx = sourceIdx;

    const char *candidates[3] = { nullptr, nullptr, nullptr };
    if (sourceIdx == 1) { // OpenStreetMap (OSM)
        candidates[0] = "/speedmap/maptiles_osm.bin";
        candidates[1] = "/speedmap/maptiles.bin";
        candidates[2] = "/speedmap/maptiles_voyager.bin";
    } else if (sourceIdx == 2) { // OSM Dark / Voyager
        candidates[0] = "/speedmap/maptiles_voyager.bin";
        candidates[1] = "/speedmap/maptiles_dark.bin";
        candidates[2] = "/speedmap/maptiles.bin";
    } else { // Carto Dark (0)
        candidates[0] = "/speedmap/maptiles.bin";
        candidates[1] = "/speedmap/maptiles_carto.bin";
        candidates[2] = "/speedmap/maptiles_osm.bin";
    }

    for (int i = 0; i < 3; i++) {
        if (!candidates[i]) continue;
        uint8_t testHdr[64];
        if (sdMgrReadBytes(candidates[i], 0, testHdr, sizeof(testHdr))) {
            if (memcmp(testHdr, MAPTILES_MAGIC, 4) == 0) {
                Serial.printf("[raster_map] Selected source %d -> file %s\n", sourceIdx, candidates[i]);
                return begin(candidates[i]);
            }
        }
    }
    Serial.printf("[raster_map] Source %d files not found, trying default /speedmap/maptiles.bin\n", sourceIdx);
    return begin("/speedmap/maptiles.bin");
}

const char *RasterMapManager::getSourceName() const {
    if (currentSourceIdx == 1) return "OpenStreetMap (OSM)";
    if (currentSourceIdx == 2) return "OSM Voyager";
    return "CartoDB Dark";
}

bool RasterMapManager::begin(const char *path) {
    if (loaded) {
        if (path && strcmp(filePath, path) == 0) return true;
        unload();
    }
    if (!path || !path[0]) return false;

    strncpy(filePath, path, sizeof(filePath) - 1);

    // 1. Read 64-byte Header
    if (!sdMgrReadBytes(filePath, 0, (uint8_t *)&header, sizeof(header))) {
        Serial.printf("[raster_map] Failed to read header from %s\n", filePath);
        return false;
    }

    if (memcmp(header.magic, MAPTILES_MAGIC, 4) != 0 || header.version != MAPTILES_VERSION) {
        Serial.printf("[raster_map] Invalid magic or version: %.4s v%u\n", header.magic, header.version);
        return false;
    }

    if (header.totalTiles == 0 || header.indexSize == 0) {
        Serial.println("[raster_map] Empty tile index");
        return false;
    }

    // 2. Allocate PSRAM for Compact Sparse Index (~31 KB for 252K tiles)
    numSparseBlocks = (header.totalTiles + kBlockEntries - 1) / kBlockEntries;
    size_t sparseBytes = numSparseBlocks * sizeof(MapTileSparseEntry);
    sparseIndex = (MapTileSparseEntry *)heap_caps_malloc(sparseBytes, MALLOC_CAP_SPIRAM);
    if (!sparseIndex) {
        sparseIndex = (MapTileSparseEntry *)malloc(sparseBytes);
    }
    if (!sparseIndex) {
        Serial.printf("[raster_map] Out of memory for sparse index (%u bytes)\n", (unsigned int)sparseBytes);
        return false;
    }

    // 3. Sequential scan through index to populate sparse index (fast 64KB buffer in PSRAM)
    const uint32_t kChunkEntries = 4096;
    MapTileIndexEntry *chunk = (MapTileIndexEntry *)heap_caps_malloc(kChunkEntries * sizeof(MapTileIndexEntry), MALLOC_CAP_SPIRAM);
    if (!chunk) chunk = (MapTileIndexEntry *)malloc(kChunkEntries * sizeof(MapTileIndexEntry));
    if (!chunk) {
        heap_caps_free(sparseIndex);
        sparseIndex = nullptr;
        return false;
    }

    uint32_t currentEntry = 0;
    while (currentEntry < header.totalTiles) {
        esp_task_wdt_reset(); // Keep watchdog alive during index scan

        uint32_t toRead = header.totalTiles - currentEntry;
        if (toRead > kChunkEntries) toRead = kChunkEntries;

        uint32_t readOffset = header.indexOffset + currentEntry * sizeof(MapTileIndexEntry);
        if (!sdMgrReadBytes(filePath, readOffset, (uint8_t *)chunk, toRead * sizeof(MapTileIndexEntry))) {
            Serial.println("[raster_map] Failed to read index chunk");
            heap_caps_free(chunk);
            heap_caps_free(sparseIndex);
            sparseIndex = nullptr;
            return false;
        }

        for (uint32_t i = 0; i < toRead; i++) {
            uint32_t absIdx = currentEntry + i;
            if (absIdx % kBlockEntries == 0) {
                int b = absIdx / kBlockEntries;
                if (b < numSparseBlocks) {
                    sparseIndex[b].startKey = chunk[i].tileKey;
                    sparseIndex[b].fileOffset = header.indexOffset + absIdx * sizeof(MapTileIndexEntry);
                    uint32_t remaining = header.totalTiles - absIdx;
                    sparseIndex[b].entryCount = (remaining < kBlockEntries) ? (uint16_t)remaining : (uint16_t)kBlockEntries;
                }
            }
        }
        currentEntry += toRead;
    }
    heap_caps_free(chunk);

    cachedBlockIdx = -1;

    // 4. Allocate LRU tile cache buffers in PSRAM (8 slots * 8KB = 64KB)
    for (int i = 0; i < kCacheSlots; i++) {
        cache[i].key = 0;
        cache[i].size = 0;
        cache[i].lastUsedMs = 0;
        cache[i].buf = (uint8_t *)heap_caps_malloc(kTileBufBytes, MALLOC_CAP_SPIRAM);
        if (!cache[i].buf) {
            cache[i].buf = (uint8_t *)malloc(kTileBufBytes);
        }
        memset(&cache[i].imgDsc, 0, sizeof(lv_image_dsc_t));
    }

    loaded = true;
    Serial.printf("[raster_map] Loaded %s: %u tiles, zoom %u..%u (sparse index: %u blocks, %u bytes PSRAM)\n",
                  filePath, (unsigned int)header.totalTiles, header.minZoom, header.maxZoom,
                  numSparseBlocks, (unsigned int)sparseBytes);
    return true;
}

bool RasterMapManager::findTileEntry(uint64_t key, MapTileIndexEntry &outEntry) const {
    if (!loaded || !sparseIndex || numSparseBlocks == 0) return false;

    // 1. Binary search on sparseIndex to find candidate block
    int low = 0;
    int high = numSparseBlocks - 1;
    int foundBlock = -1;

    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (sparseIndex[mid].startKey <= key) {
            foundBlock = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (foundBlock < 0) return false;

    // 2. Load block into blockBuf if not already cached
    const MapTileSparseEntry &block = sparseIndex[foundBlock];
    if (cachedBlockIdx != foundBlock) {
        size_t bytesToRead = block.entryCount * sizeof(MapTileIndexEntry);
        if (!sdMgrReadBytes(filePath, block.fileOffset, (uint8_t *)blockBuf, bytesToRead)) {
            return false;
        }
        cachedBlockIdx = foundBlock;
    }

    // 3. Binary search within blockBuf
    int bLow = 0;
    int bHigh = (int)block.entryCount - 1;
    while (bLow <= bHigh) {
        int mid = bLow + (bHigh - bLow) / 2;
        if (blockBuf[mid].tileKey == key) {
            outEntry = blockBuf[mid];
            return true;
        } else if (blockBuf[mid].tileKey < key) {
            bLow = mid + 1;
        } else {
            bHigh = mid - 1;
        }
    }

    return false;
}

void RasterMapManager::latLonToTile(float latDeg, float lonDeg, uint8_t zoom,
                                     uint32_t &outX, uint32_t &outY, float &outSubPxX, float &outSubPxY) {
    float n = (float)(1 << zoom);
    float x = (lonDeg + 180.0f) / 360.0f * n;

    float latRad = latDeg * (float)M_PI / 180.0f;
    float y = (1.0f - asinhf(tanf(latRad)) / (float)M_PI) / 2.0f * n;

    outX = (uint32_t)floorf(x);
    outY = (uint32_t)floorf(y);
    outSubPxX = (x - (float)outX) * 256.0f;
    outSubPxY = (y - (float)outY) * 256.0f;
}

bool RasterMapManager::getTileJpeg(uint8_t zoom, uint32_t x, uint32_t y, const uint8_t **outJpg, size_t *outSize) {
    const lv_image_dsc_t *dsc = nullptr;
    if (!getTileDsc(zoom, x, y, &dsc)) return false;
    *outJpg = dsc->data;
    *outSize = dsc->data_size;
    return true;
}

bool RasterMapManager::getTileDsc(uint8_t zoom, uint32_t x, uint32_t y, const lv_image_dsc_t **outDsc) {
    if (!loaded) return false;
    uint64_t key = ((uint64_t)zoom << 40) | ((uint64_t)x << 20) | (uint64_t)y;

    uint32_t now = millis();

    // Check LRU cache
    for (int i = 0; i < kCacheSlots; i++) {
        if (cache[i].key == key && cache[i].buf != nullptr) {
            cache[i].lastUsedMs = now;
            *outDsc = &cache[i].imgDsc;
            return true;
        }
    }

    // Lookup in index table via sparse index
    MapTileIndexEntry entry;
    if (!findTileEntry(key, entry)) return false;

    if (entry.dataSize > kTileBufBytes) return false; // oversized tiles (223 in the VN set exceed 8KB) now fit instead of rendering blank

    // Find least recently used cache slot
    int lruIdx = 0;
    uint32_t oldestTime = cache[0].lastUsedMs;
    for (int i = 1; i < kCacheSlots; i++) {
        if (cache[i].key == 0) {
            lruIdx = i;
            break;
        }
        if (cache[i].lastUsedMs < oldestTime) {
            oldestTime = cache[i].lastUsedMs;
            lruIdx = i;
        }
    }

    if (!cache[lruIdx].buf) return false;

    if (!sdMgrReadBytes(filePath, entry.fileOffset, cache[lruIdx].buf, entry.dataSize)) {
        return false;
    }

    cache[lruIdx].key = key;
    cache[lruIdx].size = entry.dataSize;
    cache[lruIdx].lastUsedMs = now;

    // Populate persistent lv_image_dsc_t with LVGL v9 requirements
    memset(&cache[lruIdx].imgDsc, 0, sizeof(lv_image_dsc_t));
    cache[lruIdx].imgDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cache[lruIdx].imgDsc.header.cf = LV_COLOR_FORMAT_RAW;
    cache[lruIdx].imgDsc.header.flags = 0;
    cache[lruIdx].imgDsc.header.w = 256;
    cache[lruIdx].imgDsc.header.h = 256;
    cache[lruIdx].imgDsc.header.stride = 256 * 3;
    cache[lruIdx].imgDsc.data_size = entry.dataSize;
    cache[lruIdx].imgDsc.data = cache[lruIdx].buf;

    *outDsc = &cache[lruIdx].imgDsc;
    return true;
}

void RasterMapManager::renderBackground(lv_layer_t *layer, float centerLatDeg, float centerLonDeg,
                                        uint8_t zoom, int canvasW, int canvasH, int anchorX, int anchorY,
                                        float scale) {
    if (!loaded) {
        static uint32_t lastTryMs = 0;
        uint32_t now = millis();
        if (now - lastTryMs > 2000) {
            lastTryMs = now;
            begin(filePath[0] ? filePath : "/speedmap/maptiles.bin");
        }
    }
    if (!loaded || !layer) return;

    if (zoom < header.minZoom) zoom = header.minZoom;
    if (zoom > header.maxZoom) zoom = header.maxZoom;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 8.0f) scale = 8.0f; // allow the zoom-fallback upscale (Dashboard::rasterZoomAndScale) when high-zoom tiles are missing

    uint32_t centerTileX = 0, centerTileY = 0;
    float subPxX = 0, subPxY = 0;
    latLonToTile(centerLatDeg, centerLonDeg, zoom, centerTileX, centerTileY, subPxX, subPxY);

    int scaledTileSize = (int)roundf(256.0f * scale);
    int originX = anchorX - (int)roundf(subPxX * scale);
    int originY = anchorY - (int)roundf(subPxY * scale);

    // Precise coverage bounds for [0, canvasW) x [0, canvasH)
    int minDx = (int)floorf((float)(-originX) / (float)scaledTileSize);
    int maxDx = (int)floorf((float)(canvasW - 1 - originX) / (float)scaledTileSize);
    int minDy = (int)floorf((float)(-originY) / (float)scaledTileSize);
    int maxDy = (int)floorf((float)(canvasH - 1 - originY) / (float)scaledTileSize);

    uint16_t scaleVal = (uint16_t)roundf(256.0f * scale);

    for (int dy = minDy; dy <= maxDy; dy++) {
        for (int dx = minDx; dx <= maxDx; dx++) {
            uint32_t tx = centerTileX + dx;
            uint32_t ty = centerTileY + dy;

            const lv_image_dsc_t *imgDsc = nullptr;
            if (getTileDsc(zoom, tx, ty, &imgDsc)) {
                int posX = originX + dx * scaledTileSize;
                int posY = originY + dy * scaledTileSize;

                lv_draw_image_dsc_t drawDsc;
                lv_draw_image_dsc_init(&drawDsc);
                drawDsc.src = imgDsc;
                drawDsc.scale_x = scaleVal;
                drawDsc.scale_y = scaleVal;
                drawDsc.antialias = 0;

                lv_area_t coords;
                coords.x1 = posX;
                coords.y1 = posY;
                coords.x2 = posX + 256 - 1;
                coords.y2 = posY + 256 - 1;

                lv_draw_image(layer, &drawDsc, &coords);
            }
        }
    }
}
