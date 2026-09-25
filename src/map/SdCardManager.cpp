#include "SdCardManager.h"
#include "pincfg.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h> // esp_task_wdt_reset() — see ensureSdMmcBegun()'s own comment
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h> // sscanf() — trip-log filename parsing in sdMgrListTripLogs()
#include <string.h>

// SD_MMC (the ESP32-S3's dedicated SDMMC hardware peripheral), NOT SD.h +
// SPIClass — see pincfg.h's SD_MMC_CLK_PIN/CMD_PIN/D0_PIN comment for the
// full story: this board wires its TF slot to 1-bit SD_MMC (3 signals,
// no CS pin exists in this mode), confirmed from the vendor-adjacent demo
// project's own working code (refob/Arduino_JC3248W535_LVGL9.4). An earlier
// SPI-based guess (wrong protocol, not just wrong pins) caused two distinct
// real-hardware regressions by forcing extra traffic onto the SPI2/SPI3
// peripherals the display and touch driver already depend on — SD_MMC's
// dedicated peripheral shares neither, which is what actually fixes it.

// Standard reflected CRC-32 (polynomial 0xEDB88320) — the same algorithm
// Python's zlib.crc32 uses, so build_speedmap.py's `zlib.crc32(index_bytes)`
// and this function agree on the same input bytes without either side
// needing the other's library. Hand-rolled rather than an ESP-IDF ROM CRC
// API: this project doesn't otherwise depend on ROM-level APIs, and a
// ~20-line table-based implementation is cheap enough not to need one.
static uint32_t crc32(const uint8_t *data, size_t len) {
    static uint32_t table[256];
    static bool tableInit = false;
    if (!tableInit) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableInit = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Serializes every function below against the other (log/TripLogger.cpp's
// task calling sdMgrAppendLine() while map/SpeedLimitManager.cpp's task
// calls sdMgrReadTile(), say) — added 2026-09-16 once TripLogger became a
// second real caller task; SD_MMC is one physical peripheral, not reentrant
// across FreeRTOS tasks without a lock. RAII so every early `return false`
// already scattered through this file (unchanged) still releases it.
static SemaphoreHandle_t sdMutex = NULL;
struct SdLock {
    SdLock() {
        if (!sdMutex) sdMutex = xSemaphoreCreateMutex();
        xSemaphoreTake(sdMutex, portMAX_DELAY);
    }
    ~SdLock() { xSemaphoreGive(sdMutex); }
};

static bool mounted = false;
static SpeedMapMetadata metadata;

// Loaded once at mount() time and kept in RAM for the whole session.
// Allocated EXACTLY metadata.tileCount entries in PSRAM (not a fixed cap in
// static internal RAM) — sized 2026-09-16 for a real Northern-Vietnam
// extract (tools/map_builder/, classified roads only: motorway..tertiary)
// which produced 62081 tiles, dwarfing the earlier Hanoi-core test's 165.
// At 28 bytes/entry that's ~1.7MB, which would have been impossible in this
// board's 320KB internal RAM (the previous fixed 512-entry static array's
// whole reason for existing) but is a rounding error against 8MB PSRAM —
// same "move it to PSRAM, not internal RAM" reasoning
// SpeedLimitManager.cpp's tile segment CACHE already uses. No arbitrary cap
// to bump again next time the region grows: this just allocates what
// metadata.tileCount says, so the only real ceiling is PSRAM capacity
// itself (~285000 tiles at 8MB, far beyond any realistic single-country
// extract).
static TileIndexEntry *tileIndex = NULL;
static int tileIndexCount = 0;

// Loaded once, independently of sdMgrMount()'s own tile-index loading (see
// this array's own header comment for why) — a flat CameraPoint[], small
// enough (real speed-camera counts are low thousands at most, 16
// bytes/entry) that PSRAM vs. internal RAM doesn't matter the way it does
// for tileIndex above; PSRAM chosen anyway for consistency with everything
// else this module keeps resident.
static CameraPoint *cameraPoints = NULL;
static int cameraPointCount = 0;
static TrafficSignPoint *trafficSigns = NULL;
static int trafficSignCount = 0;

// Road Names Database in PSRAM
static uint16_t roadNameCount = 0;
static uint32_t *roadNameOffsets = NULL;
static char *roadNamePool = NULL;
static size_t roadNamePoolSize = 0;

bool sdMgrIsAvailable() { return mounted; }

// Brings up the underlying SD_MMC peripheral exactly once, however many of
// sdMgrMount()/sdMgrAppendLine() end up calling it first — added 2026-09-16
// when log/TripLogger.cpp's own task became a second caller that shouldn't
// have to wait on (or race) map/SpeedLimitManager.cpp's task calling
// sdMgrMount() first just to get a working card. Distinct from `mounted`
// above: that flag additionally requires a valid speedmap database
// (metadata.bin etc., see sdMgrMount() below), whereas this only means "the
// SD_MMC peripheral itself is up," which sdMgrAppendLine() alone needs.
static bool sdMmcBegun = false;
static uint32_t lastFailedAttemptMs = 0;
// Confirmed on real hardware 2026-09-16 (no SD card inserted): without this,
// log/TripLogger.cpp's task retries sdMgrAppendLine() every 200ms forever
// while its header write keeps failing, and EVERY one of those calls re-ran
// this whole function's ~400ms of blocking delay()s plus a full
// SD_MMC.begin() attempt — a tight, permanent loop hammering the peripheral
// and spamming the log roughly every 1.6s with no card ever going to appear.
// 30s is generous enough that a card inserted after boot is still picked up
// reasonably promptly, while cutting the retry rate by >100x when none is
// present at all.
static const uint32_t kRetryCooldownMs = 30000;

static bool ensureSdMmcBegun() {
    if (sdMmcBegun) return true;
    uint32_t now = millis();
    // lastFailedAttemptMs starts at 0, which would look identical to "an
    // attempt just failed at time 0" — but millis() reads >0 by the time any
    // task is far enough into its loop to call this, so a real first attempt
    // is never mistakenly skipped by this check.
    if (lastFailedAttemptMs != 0 && now - lastFailedAttemptMs < kRetryCooldownMs) return false;
    lastFailedAttemptMs = now; // set up front — every early return below is a failed attempt

    // Tear the peripheral down before priming/re-initialising it (added
    // 2026-09-21). Confirmed on real hardware: after a run of rapid
    // upload-triggered soft resets, SD_MMC.begin() started failing with
    // sdmmc_init_ocr / send_op_cond error 0x107 ("no card detected") on
    // EVERY retry for a whole boot, and only a true power cycle brought it
    // back — the exact same class of "a soft reset resets the ESP32, NOT
    // the peripheral" failure this project already hit with the AXS15231B
    // touch controller. A soft reset can leave the SDMMC host half-
    // initialised from the previous run, and begin() on top of that can't
    // recover; end() first puts it back to a known state. Deliberately
    // BEFORE the clock-priming below, not after — end() reconfigures these
    // same pins, so priming first would just be undone. Harmless when
    // nothing was ever begun (end() on an un-begun host is a no-op).
    SD_MMC.end();
    delay(10);

    // Clock-priming sequence lifted verbatim from the vendor-adjacent demo
    // (mp3_player.ino) — sends the card a couple of clock transitions with
    // CMD held high (pulled up) before SD_MMC.begin() proper, a well-known
    // SD initialization quirk (cards can need "warm-up" clocks with
    // CS/CMD high to settle into the right mode). Kept even though this
    // project can't independently verify it's load-bearing on this exact
    // board, on the same "a hard-won hardware fix from someone who tested
    // it stays unless disproven" principle this project applies to its own
    // discoveries (e.g. the touch controller's 100kHz requirement).
    pinMode(SD_MMC_CMD_PIN, INPUT_PULLUP);
    pinMode(SD_MMC_D0_PIN, INPUT_PULLUP);
    pinMode(SD_MMC_CLK_PIN, OUTPUT);
    digitalWrite(SD_MMC_CLK_PIN, LOW);
    delay(200);
    digitalWrite(SD_MMC_CLK_PIN, HIGH);
    delay(200);

    if (!SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN)) {
        Serial.println("[sdmgr] SD_MMC.setPins() failed");
        return false;
    }
    // esp_task_wdt_reset() calls threaded through this function's 3-stage
    // retry — added 2026-09-22 after a REAL, reproducible field crash:
    // "Task watchdog got triggered ... speedLimitTask ... Aborting." A
    // failing card makes each of the 3 SD_MMC.begin() attempts below block
    // for real time (its own internal command retries at the hardware
    // level, worse at the slower fallback frequencies), on top of the
    // explicit delay()s already here — confirmed on hardware to add up to
    // several seconds for one full failed call. speedLimitTaskFn's own
    // retry-mount logic calls sdMgrMount() (which calls this) directly
    // inline in its loop, before that loop's own esp_task_wdt_reset() — so
    // a single slow call here could burn through the whole per-task
    // watchdog budget before ever reaching it. A no-op (returns an error
    // code, nothing worse) if the calling task was never registered via
    // esp_task_wdt_add(), so this is safe from every caller of
    // ensureSdMmcBegun() (map/SpeedLimitManager.cpp, log/TripLogger.cpp,
    // etc.), not just the one that actually crashed.
    esp_task_wdt_reset();
    if (SD_MMC.begin("/sdmmc", true, false, SDMMC_FREQ_DEFAULT)) {
        sdMmcBegun = true;
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[sdmgr] SD Card mounted successfully! Size: %llu MB, Type: %d\n", cardSize, SD_MMC.cardType());
        return true;
    }
    SD_MMC.end();
    delay(100);
    esp_task_wdt_reset();
    if (SD_MMC.begin("/sdmmc", true, false, 10000)) {
        sdMmcBegun = true;
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[sdmgr] SD Card mounted at 10MHz fallback! Size: %llu MB\n", cardSize);
        return true;
    }
    SD_MMC.end();
    delay(100);
    esp_task_wdt_reset();
    if (SD_MMC.begin("/sdmmc", true, false, SDMMC_FREQ_PROBING)) {
        sdMmcBegun = true;
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[sdmgr] SD Card mounted at 400kHz fallback! Size: %llu MB\n", cardSize);
        return true;
    }
    esp_task_wdt_reset();
    Serial.println("[sdmgr] SD_MMC.begin() failed — no card detected");
    return false;
}

bool sdMgrMount() {
    SdLock lock;
    if (mounted) return true;
    mounted = false;
    if (!ensureSdMmcBegun()) return false;

    // Paths are relative to the SD_MMC filesystem's own root, NOT prefixed
    // with the "/sdmmc" mountpoint string passed to begin() above — that
    // string only registers the VFS mount internally (confirmed against
    // the vendor demo's own usage: it does SD_MMC.begin("/sdmmc", ...) but
    // then SD_MMC.open("/music"), not SD_MMC.open("/sdmmc/music")).
    File metaFile = SD_MMC.open("/speedmap/metadata.bin");
    if (!metaFile) {
        Serial.println("[sdmgr] /speedmap/metadata.bin not found");
        return false;
    }
    size_t got = metaFile.read((uint8_t *)&metadata, sizeof(metadata));
    metaFile.close();
    if (got != sizeof(metadata)) {
        Serial.printf("[sdmgr] metadata.bin too short (%u of %u bytes) — MAP ERROR\n", (unsigned)got,
                      (unsigned)sizeof(metadata));
        return false;
    }
    if (memcmp(metadata.magic, SPEEDMAP_MAGIC, 4) != 0) {
        Serial.println("[sdmgr] metadata.bin magic mismatch — not a speedmap database, MAP ERROR");
        return false;
    }
    if (metadata.formatVersion != SPEEDMAP_FORMAT_VERSION) {
        Serial.printf("[sdmgr] metadata.bin format_version=%u, firmware supports %u — MAP FORMAT ERROR\n",
                      metadata.formatVersion, SPEEDMAP_FORMAT_VERSION);
        return false;
    }

    File idxFile = SD_MMC.open("/speedmap/index.bin");
    if (!idxFile) {
        Serial.println("[sdmgr] /speedmap/index.bin not found — MAP ERROR");
        return false;
    }
    size_t idxSize = idxFile.size();
    if (idxSize != metadata.tileCount * sizeof(TileIndexEntry)) {
        Serial.printf("[sdmgr] index.bin size %u doesn't match tileCount*%u — MAP ERROR\n", (unsigned)idxSize,
                      (unsigned)sizeof(TileIndexEntry));
        idxFile.close();
        return false;
    }
    // Free a previous mount's buffer before allocating a new one — sdMgrMount()
    // isn't currently called more than once per boot, but this keeps a
    // second call (e.g. a future "reload map" feature) from leaking PSRAM
    // instead of silently assuming it never happens.
    if (tileIndex) {
        heap_caps_free(tileIndex);
        tileIndex = NULL;
    }
    tileIndex = (TileIndexEntry *)heap_caps_malloc(idxSize, MALLOC_CAP_SPIRAM);
    if (!tileIndex) {
        Serial.printf("[sdmgr] PSRAM allocation for %u tile index entries (%u bytes) skipped - using on-demand file index (0 KB PSRAM)\n",
                      (unsigned)metadata.tileCount, (unsigned)idxSize);
        idxFile.close();
        tileIndexCount = (int)metadata.tileCount;
    } else {
        tileIndexCount = (int)metadata.tileCount;
        size_t idxGot = idxFile.read((uint8_t *)tileIndex, idxSize);
        idxFile.close();
        if (idxGot != idxSize) {
            Serial.println("[sdmgr] short read on index.bin - falling back to file index");
            heap_caps_free(tileIndex);
            tileIndex = NULL;
        } else {
            uint32_t computedCrc = crc32((const uint8_t *)tileIndex, idxSize);
            if (computedCrc != metadata.crc32) {
                Serial.printf("[sdmgr] index.bin CRC mismatch (computed 0x%08lX, expected 0x%08lX) - falling back to file index\n",
                              (unsigned long)computedCrc, (unsigned long)metadata.crc32);
                heap_caps_free(tileIndex);
                tileIndex = NULL;
            }
        }
    }

    Serial.printf("[sdmgr] mounted OK — region=%.16s version=%.16s tiles=%d\n", metadata.region, metadata.mapVersion,
                  tileIndexCount);
    mounted = true;

    // cameras.bin — loaded independently of the metadata/index validation
    // above, and never fails sdMgrMount() itself: this is an OPTIONAL
    // extra (see CameraPoint's own SpeedMapFormat.h comment), so a card
    // built before it existed, or a region export with genuinely zero
    // speed cameras, both correctly end up with cameraPointCount==0
    // rather than a MAP ERROR.
    if (cameraPoints) {
        heap_caps_free(cameraPoints);
        cameraPoints = NULL;
        cameraPointCount = 0;
    }
    File camFile = SD_MMC.open("/speedmap/cameras.bin");
    if (camFile) {
        size_t camSize = camFile.size();
        int n = (int)(camSize / sizeof(CameraPoint));
        if (n > 0) {
            cameraPoints = (CameraPoint *)heap_caps_malloc(n * sizeof(CameraPoint), MALLOC_CAP_SPIRAM);
            if (cameraPoints) {
                size_t got = camFile.read((uint8_t *)cameraPoints, n * sizeof(CameraPoint));
                cameraPointCount = (int)(got / sizeof(CameraPoint));
            } else {
                Serial.println("[sdmgr] PSRAM allocation for camera points failed — continuing with zero cameras");
            }
        }
        camFile.close();
    }
    Serial.printf("[sdmgr] cameras.bin: %d speed camera(s) loaded\n", cameraPointCount);

    // signs.bin — loaded into PSRAM for instant offline query
    if (trafficSigns) {
        heap_caps_free(trafficSigns);
        trafficSigns = NULL;
        trafficSignCount = 0;
    }
    File signFile = SD_MMC.open("/speedmap/signs.bin");
    if (signFile) {
        TrafficSignHeader hdr;
        if (signFile.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) &&
            memcmp(hdr.magic, SIGN_MAGIC, 4) == 0 && hdr.signCount > 0) {
            trafficSigns = (TrafficSignPoint *)heap_caps_malloc(hdr.signCount * sizeof(TrafficSignPoint), MALLOC_CAP_SPIRAM);
            if (trafficSigns) {
                size_t got = signFile.read((uint8_t *)trafficSigns, hdr.signCount * sizeof(TrafficSignPoint));
                trafficSignCount = (int)(got / sizeof(TrafficSignPoint));
            } else {
                Serial.println("[sdmgr] PSRAM allocation for traffic signs failed");
            }
        }
        signFile.close();
    }
    Serial.printf("[sdmgr] signs.bin: %d traffic sign(s) loaded\n", trafficSignCount);

    // names.bin - loaded into PSRAM for road name display
    if (roadNameOffsets) { heap_caps_free(roadNameOffsets); roadNameOffsets = NULL; }
    if (roadNamePool) { heap_caps_free(roadNamePool); roadNamePool = NULL; }
    roadNameCount = 0;
    roadNamePoolSize = 0;

    File nameFile = SD_MMC.open("/speedmap/names.bin");
    if (nameFile) {
        char magic[4];
        uint16_t version = 0, count = 0;
        uint32_t totalBytes = 0, reserved = 0;
        if (nameFile.read((uint8_t *)magic, 4) == 4 && memcmp(magic, "VNNM", 4) == 0) {
            nameFile.read((uint8_t *)&version, 2);
            nameFile.read((uint8_t *)&count, 2);
            nameFile.read((uint8_t *)&totalBytes, 4);
            nameFile.read((uint8_t *)&reserved, 4);

            if (count > 0 && totalBytes > 0 && count < 65535 && totalBytes < 2000000) {
                roadNameOffsets = (uint32_t *)heap_caps_malloc(count * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
                roadNamePool = (char *)heap_caps_malloc(totalBytes, MALLOC_CAP_SPIRAM);
                if (roadNameOffsets && roadNamePool) {
                    nameFile.read((uint8_t *)roadNameOffsets, count * sizeof(uint32_t));
                    nameFile.read((uint8_t *)roadNamePool, totalBytes);
                    // Defensive: force a terminator on the very last byte so any
                    // string returned by sdMgrGetRoadName() (which only checks that
                    // its START offset is < poolSize) is guaranteed to be
                    // NUL-terminated within the buffer. Without this, a truncated
                    // or corrupt names.bin (e.g. an interrupted OTA) whose last
                    // string lacks a terminator would let strncpy/label rendering
                    // read past the pool end — an OOB read on the constantly-used
                    // road-name display path. Costs one string entry at worst.
                    roadNamePool[totalBytes - 1] = '\0';
                    roadNameCount = count;
                    roadNamePoolSize = totalBytes;
                    Serial.printf("[sdmgr] names.bin: %d street names loaded (%u bytes string pool)\n",
                                  roadNameCount, (unsigned int)roadNamePoolSize);
                } else {
                    if (roadNameOffsets) { heap_caps_free(roadNameOffsets); roadNameOffsets = NULL; }
                    if (roadNamePool) { heap_caps_free(roadNamePool); roadNamePool = NULL; }
                    Serial.println("[sdmgr] PSRAM allocation for road names failed");
                }
            }
        }
        nameFile.close();
    }

    return true;
}

bool sdMgrGetMetadata(SpeedMapMetadata &out) {
    SdLock lock;
    if (!mounted) return false;
    out = metadata;
    return true;
}

bool sdMgrGetCameras(const CameraPoint **out, int *outCount) {
    SdLock lock;
    *out = cameraPoints;
    *outCount = cameraPointCount;
    return true; // 0 cameras is a normal, successful result — see this array's own comment
}

bool sdMgrGetSigns(const TrafficSignPoint **out, int *outCount) {
    SdLock lock;
    *out = trafficSigns;
    *outCount = trafficSignCount;
    return true;
}

bool sdMgrGetIndex(const TileIndexEntry **out, int *outCount) {
    SdLock lock;
    if (!mounted) return false;
    *out = tileIndex;
    *outCount = tileIndexCount;
    return true;
}


bool sdMgrReadBytes(const char *path, uint32_t offset, uint8_t *outBuf, size_t len) {
    SdLock lock;
    if (!ensureSdMmcBegun() || !path || !outBuf || len == 0) return false;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    if (offset > 0 && !f.seek(offset)) {
        f.close();
        return false;
    }
    size_t got = f.read(outBuf, len);
    f.close();
    return got == len;
}

bool sdMgrReadTile(const TileIndexEntry &entry, RoadSegment *outBuf, int maxSegments, int *outCount) {
    SdLock lock;
    *outCount = 0;
    if (!mounted) return false;

    // One shared blob for every tile (format V2) — opened fresh per call
    // rather than kept as a persistent handle, same "no extra state to get
    // wrong" reasoning every other read in this file already follows; SD_MMC
    // open() is cheap compared to the seek+read that follows anyway.
    File f = SD_MMC.open("/speedmap/tiles.bin");
    if (!f) {
        Serial.println("[sdmgr] /speedmap/tiles.bin not found (treating tile as empty)");
        return false;
    }
    if (!f.seek(entry.fileOffset)) {
        Serial.printf("[sdmgr] seek to offset %lu in tiles.bin failed\n", (unsigned long)entry.fileOffset);
        f.close();
        return false;
    }
    int available = (int)(entry.fileSize / sizeof(RoadSegment));
    int toRead = available < maxSegments ? available : maxSegments;
    size_t got = f.read((uint8_t *)outBuf, toRead * sizeof(RoadSegment));
    f.close();
    *outCount = (int)(got / sizeof(RoadSegment));
    return true;
}

bool sdMgrDumpFileToSerial(const char *path) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return false;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[sdmgr] dump: open failed: %s\n", path);
        return false;
    }
    Serial.printf("-----BEGIN FILE %s (%u bytes)-----\n", path, (unsigned)f.size());
    uint8_t buf[256];
    while (true) {
        size_t got = f.read(buf, sizeof(buf));
        if (got == 0) break;
        Serial.write(buf, got);
    }
    Serial.println();
    Serial.printf("-----END FILE %s-----\n", path);
    f.close();
    return true;
}

void sdMgrSummarizeTripLogs(uint32_t firstSessionId, uint32_t lastSessionId) {
    SdLock lock;
    if (!ensureSdMmcBegun()) {
        Serial.println("[sdmgr] summarize: SD_MMC not available");
        return;
    }
    for (uint32_t id = firstSessionId; id <= lastSessionId; id++) {
        char path[48];
        snprintf(path, sizeof(path), "/triplog/session_%04lu.csv", (unsigned long)id);
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) continue; // this session number just never got a file (e.g. logging was off that boot) — not an error
        size_t sizeBytes = f.size();
        int lineCount = 0;
        float maxEgoKmh = 0;
        bool anySpeeding = false, anyEvent = false;
        f.readStringUntil('\n'); // skip header
        while (f.available()) {
            String line = f.readStringUntil('\n');
            if (line.length() == 0) continue;
            lineCount++;
            // tMs,egoKmh,limitValid,limitKmh,speeding,cameraAheadM,signType,signDistM,event
            // (log/TripLogger.cpp's schema — updated 2026-09-21 when radar
            // removal replaced its old activeTargets/primDistM/TTC columns
            // with speed-limit/camera/sign fields)
            int c1 = line.indexOf(',');
            int c2 = line.indexOf(',', c1 + 1);
            int c3 = line.indexOf(',', c2 + 1);
            int c4 = line.indexOf(',', c3 + 1);
            if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) continue;
            float egoKmh = line.substring(c1 + 1, c2).toFloat();
            int speeding = line.substring(c3 + 1, c4).toInt();
            if (egoKmh > maxEgoKmh) maxEgoKmh = egoKmh;
            if (speeding > 0) anySpeeding = true;
            if (line.endsWith("EVENT")) anyEvent = true;
        }
        f.close();
        Serial.printf("[sdmgr] session_%04lu: %uB lines=%d maxEgoKmh=%.1f anySpeeding=%d anyEvent=%d\n",
                      (unsigned long)id, (unsigned)sizeBytes, lineCount, (double)maxEgoKmh, (int)anySpeeding,
                      (int)anyEvent);
    }
}

int sdMgrListTripLogs(uint32_t *outIds, uint32_t *outSizes, int maxCount) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return 0;
    File dir = SD_MMC.open("/triplog");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        // name() is the bare filename on this core's SD_MMC (no directory
        // prefix); parse "session_NNNN.csv" and skip anything else that
        // happens to be in the directory.
        const char *name = f.name();
        const char *lastSlash = strrchr(name, '/');
        if (lastSlash) name = lastSlash + 1;
        unsigned id = 0;
        if (!f.isDirectory() && sscanf(name, "session_%u.csv", &id) == 1) {
            if (n < maxCount) {
                outIds[n] = (uint32_t)id;
                outSizes[n] = (uint32_t)f.size();
                n++;
            } else {
                // Keep the NEWEST maxCount, not the first maxCount the
                // directory happens to hand back (added 2026-09-21 after a
                // real card with 181 sessions on it returned ids 55-118 —
                // i.e. only old bench-test boots, with the drive anyone
                // actually wants never listed at all). Session ids only
                // ever increase, so "newest" is just "largest id": once
                // full, evict the smallest whenever a larger one shows up.
                int smallest = 0;
                for (int i = 1; i < n; i++)
                    if (outIds[i] < outIds[smallest]) smallest = i;
                if ((uint32_t)id > outIds[smallest]) {
                    outIds[smallest] = (uint32_t)id;
                    outSizes[smallest] = (uint32_t)f.size();
                }
            }
        }
        f.close();
    }
    dir.close();
    return n;
}

int sdMgrDeleteAllTripLogs() {
    SdLock lock;
    if (!ensureSdMmcBegun()) return -1;
    File dir = SD_MMC.open("/triplog");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }
    // Collect session IDs first (4 bytes each, not full path strings — keeps
    // internal RAM tiny), THEN remove: deleting while an openNextFile()
    // iteration is live is unreliable on SD_MMC.
    static uint32_t ids[128];
    int n = 0;
    for (File f = dir.openNextFile(); f && n < 128; f = dir.openNextFile()) {
        const char *name = f.name();
        const char *lastSlash = strrchr(name, '/');
        const char *bare = lastSlash ? lastSlash + 1 : name;
        unsigned id = 0;
        if (!f.isDirectory() && sscanf(bare, "session_%u.csv", &id) == 1) ids[n++] = (uint32_t)id;
        f.close();
    }
    dir.close();
    int deleted = 0;
    char path[40];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "/triplog/session_%04lu.csv", (unsigned long)ids[i]);
        if (SD_MMC.remove(path)) deleted++;
    }
    Serial.printf("[sdmgr] deleted %d/%d trip log(s)\n", deleted, n);
    return deleted;
}

int sdMgrReadFileChunk(const char *path, size_t offset, uint8_t *buf, size_t bufSize) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return -1;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return -1;
    if (offset >= f.size()) {
        f.close();
        return 0;
    }
    if (!f.seek(offset)) {
        f.close();
        return -1;
    }
    int got = (int)f.read(buf, bufSize);
    f.close();
    return got;
}

bool sdMgrAppendLine(const char *path, const char *line) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return false;

    // FILE_APPEND creates the file if missing, but NOT missing parent
    // directories — SD_MMC.mkdir() one level up first if the target isn't
    // at the filesystem root. Single-level only (this project's only caller,
    // log/TripLogger.cpp, uses one fixed "/triplog" directory) — not a
    // recursive mkdir -p, since there's no current need for nested paths.
    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        char dir[64];
        size_t dirLen = (size_t)(slash - path);
        if (dirLen < sizeof(dir)) {
            memcpy(dir, path, dirLen);
            dir[dirLen] = '\0';
            if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);
        }
    }

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) {
        Serial.printf("[sdmgr] append open failed: %s\n", path);
        return false;
    }
    f.println(line);
    f.close();
    return true;
}

// Append raw bytes to a file (creates it + one-level parent dir if missing),
// mutex-guarded like the rest of this module. Open-append-close per call so the
// SD mutex is only held for one chunk at a time — the map-matcher task can still
// read tiles between chunks during a long online data download (net/
// DataUpdater.cpp). Returns true only if the full len was written.
bool sdMgrAppendBytes(const char *path, const uint8_t *buf, size_t len) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return false;
    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        char dir[64];
        size_t dirLen = (size_t)(slash - path);
        if (dirLen < sizeof(dir)) {
            memcpy(dir, path, dirLen);
            dir[dirLen] = '\0';
            if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);
        }
    }
    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) return false;
    size_t wrote = f.write(buf, len);
    f.close();
    return wrote == len;
}

bool sdMgrRemove(const char *path) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return false;
    if (!SD_MMC.exists(path)) return true; // already gone = success
    return SD_MMC.remove(path);
}

bool sdMgrRename(const char *from, const char *to) {
    SdLock lock;
    if (!ensureSdMmcBegun()) return false;
    SD_MMC.remove(to); // rename won't overwrite an existing target on some FS impls
    return SD_MMC.rename(from, to);
}

bool sdMgrFindTileEntry(uint32_t tileId, TileIndexEntry *outEntry) {
    if (!mounted || !outEntry) return false;
    SdLock lock;

    // Fast path: in-PSRAM array
    if (tileIndex) {
        int lo = 0, hi = tileIndexCount - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (tileIndex[mid].tileId == tileId) {
                *outEntry = tileIndex[mid];
                return true;
            } else if (tileIndex[mid].tileId < tileId) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return false;
    }

    // Direct file-based binary search on /speedmap/index.bin (0 KB PSRAM)
    if (!ensureSdMmcBegun()) return false;
    File f = SD_MMC.open("/speedmap/index.bin", FILE_READ);
    if (!f) return false;

    int lo = 0, hi = tileIndexCount - 1;
    bool found = false;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (!f.seek((uint32_t)mid * sizeof(TileIndexEntry))) break;
        TileIndexEntry ent;
        if (f.read((uint8_t *)&ent, sizeof(ent)) != sizeof(ent)) break;
        if (ent.tileId == tileId) {
            *outEntry = ent;
            found = true;
            break;
        } else if (ent.tileId < tileId) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    f.close();
    return found;
}

const char *sdMgrGetRoadName(uint16_t nameId) {
    if (nameId == 0 || nameId > roadNameCount || !roadNameOffsets || !roadNamePool) {
        return "";
    }
    uint32_t offset = roadNameOffsets[nameId - 1];
    if (offset < roadNamePoolSize) {
        return roadNamePool + offset;
    }
    return "";
}

const char *sdMgrGetSegmentRoadName(uint32_t segId) {
    static uint32_t sLastSegId = 0xFFFFFFFFu;
    static char sLastRoadName[64] = {0};

    if (segId == 0) return "";
    if (segId == sLastSegId) return sLastRoadName;

    uint16_t nameId = 0;
    bool ok = sdMgrReadBytes("/speedmap/seg_names.bin", segId * sizeof(uint16_t), (uint8_t *)&nameId, sizeof(nameId));
    if (ok && nameId > 0) {
        const char *name = sdMgrGetRoadName(nameId);
        if (name && name[0]) {
            sLastSegId = segId;
            strncpy(sLastRoadName, name, sizeof(sLastRoadName) - 1);
            sLastRoadName[sizeof(sLastRoadName) - 1] = '\0';
            return sLastRoadName;
        }
    }

    sLastSegId = segId;
    sLastRoadName[0] = '\0';
    return "";
}
