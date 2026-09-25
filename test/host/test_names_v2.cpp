// Host test for the names.bin v2 (uint32) format + UTF-8-safe truncation added
// 2026-09-25 (map/SdCardManager.cpp). It writes a v2 names.bin + seg_names.bin
// exactly as the firmware documents them, then parses them back with the SAME
// logic the firmware loader/reader uses, verifying:
//   * v2 header (u32 count) + u32 offsets + NUL-terminated UTF-8 pool round-trip
//   * seg_names.bin as u32-per-segment maps segId -> nameId -> string
//   * a name id beyond 65 535 works (the whole point of v2)
//   * utf8SafeCopy() never splits a multi-byte Vietnamese character
//
// Build+run (from project root):
//   "<g++>" -std=c++17 test/host/test_names_v2.cpp -o .tmpwork/nmtest && .tmpwork/nmtest
//
// The parser + utf8SafeCopy below are copied VERBATIM from SdCardManager.cpp so
// this exercises the real algorithm (that file itself can't be host-compiled — it
// pulls in SD_MMC/Arduino).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } } while(0)

// ---- VERBATIM copy of utf8SafeCopy() from SdCardManager.cpp ----
static void utf8SafeCopy(char *dst, size_t dstCap, const char *src) {
    if (dstCap == 0) return;
    size_t max = dstCap - 1;
    size_t len = 0;
    while (src[len] != '\0' && len < max) len++;
    if (src[len] != '\0') {
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// ---- in-memory image of what the firmware loads from names.bin ----
static uint32_t roadNameCount = 0;
static std::vector<uint32_t> roadNameOffsets;
static std::vector<char> roadNamePool;
static size_t roadNamePoolSize = 0;
static uint8_t segNameWidth = 2;

// Mirrors sdMgrGetRoadName() (now uint32).
static const char *getRoadName(uint32_t nameId) {
    if (nameId == 0 || nameId > roadNameCount || roadNameOffsets.empty() || roadNamePool.empty()) return "";
    uint32_t offset = roadNameOffsets[nameId - 1];
    if (offset < roadNamePoolSize) return roadNamePool.data() + offset;
    return "";
}

// ---- generate a v2 names.bin + seg_names.bin, then parse them back ----
int main() {
    printf("[names v2] format round-trip + UTF-8 truncation\n");

    // A name set that deliberately crosses the old 65 535 ceiling.
    const uint32_t N = 70000;
    std::vector<std::string> names(N);
    for (uint32_t i = 0; i < N; i++) names[i] = "R" + std::to_string(i);
    names[0] = "Đường Nguyễn Trãi";       // Vietnamese, multi-byte
    names[N - 1] = "Đại lộ Thăng Long";   // last entry, > 65 535

    // Build the pool + offsets (1-based ids; offsets are byte positions).
    std::vector<uint32_t> offs(N);
    std::string pool;
    for (uint32_t i = 0; i < N; i++) { offs[i] = (uint32_t)pool.size(); pool += names[i]; pool.push_back('\0'); }

    // --- write names.bin v2 ---
    FILE *f = fopen(".tmpwork/names.bin", "wb");
    if (!f) { printf("  FAIL: cannot open .tmpwork/names.bin (mkdir .tmpwork first)\n"); return 1; }
    uint16_t ver = 2, res16 = 0; uint32_t cnt = N, poolBytes = (uint32_t)pool.size();
    fwrite("VNNM", 1, 4, f); fwrite(&ver, 2, 1, f);
    fwrite(&cnt, 4, 1, f); fwrite(&poolBytes, 4, 1, f); fwrite(&res16, 2, 1, f);
    fwrite(offs.data(), 4, N, f); fwrite(pool.data(), 1, pool.size(), f);
    fclose(f);

    // --- write seg_names.bin as u32 per segment; segId s -> nameId ---
    // seg 10 -> name 1, seg 11 -> name N (the >65535 one), seg 12 -> name 0 (none)
    std::vector<uint32_t> segNames(13, 0);
    segNames[10] = 1; segNames[11] = N; segNames[12] = 0;
    f = fopen(".tmpwork/seg_names.bin", "wb");
    fwrite(segNames.data(), 4, segNames.size(), f);
    fclose(f);

    // ================= parse names.bin exactly like the firmware =================
    f = fopen(".tmpwork/names.bin", "rb");
    char magic[4]; uint16_t version = 0; uint32_t count = 0, totalBytes = 0;
    CHECK(fread(magic, 1, 4, f) == 4 && memcmp(magic, "VNNM", 4) == 0, "magic VNNM");
    fread(&version, 2, 1, f);
    if (version >= 2) {
        uint16_t r16 = 0; fread(&count, 4, 1, f); fread(&totalBytes, 4, 1, f); fread(&r16, 2, 1, f);
        segNameWidth = 4;
    } else {
        uint16_t c16 = 0; uint32_t r32 = 0; fread(&c16, 2, 1, f); fread(&totalBytes, 4, 1, f); fread(&r32, 4, 1, f);
        count = c16; segNameWidth = 2;
    }
    CHECK(version == 2, "parsed version == 2");
    CHECK(count == N, "parsed count == 70000 (past the u16 ceiling)");
    CHECK(segNameWidth == 4, "seg id width is 4 for v2");
    CHECK(count > 0 && totalBytes > 0 && count < 5000000u && totalBytes < 16000000u, "passes firmware sanity ceilings");
    roadNameOffsets.resize(count); roadNamePool.resize(totalBytes);
    fread(roadNameOffsets.data(), 4, count, f);
    fread(roadNamePool.data(), 1, totalBytes, f);
    roadNamePool[totalBytes - 1] = '\0'; // firmware's NUL hardening
    roadNameCount = count; roadNamePoolSize = totalBytes;
    fclose(f);

    // ================= seg_names.bin lookup like the firmware =================
    auto segRoadName = [&](uint32_t segId) -> std::string {
        uint32_t nameId = 0;
        FILE *sf = fopen(".tmpwork/seg_names.bin", "rb");
        fseek(sf, (long)(segId * segNameWidth), SEEK_SET);
        fread(&nameId, segNameWidth, 1, sf); // reads segNameWidth bytes into a u32 (LE)
        fclose(sf);
        char buf[64];
        if (nameId > 0) utf8SafeCopy(buf, sizeof(buf), getRoadName(nameId));
        else buf[0] = '\0';
        return std::string(buf);
    };

    CHECK(segRoadName(10) == "Đường Nguyễn Trãi", "seg 10 -> name 1 (Vietnamese) via u32 seg_names");
    CHECK(segRoadName(11) == "Đại lộ Thăng Long", "seg 11 -> name 70000 (>65535) resolves");
    CHECK(segRoadName(12) == "", "seg 12 -> nameId 0 -> empty");

    // ================= UTF-8-safe truncation =================
    // "Đường" = Đ(2B) ư(3B) ờ(3B) n(1B) g(1B). Truncate into small buffers and
    // confirm we never leave a partial (invalid) multi-byte sequence.
    auto validUtf8 = [](const char *s) -> bool {
        const unsigned char *p = (const unsigned char *)s;
        while (*p) {
            int n = (*p < 0x80) ? 1 : (*p >> 5) == 0x6 ? 2 : (*p >> 4) == 0xE ? 3 : (*p >> 3) == 0x1E ? 4 : -1;
            if (n < 0) return false;
            for (int k = 1; k < n; k++) if ((p[k] & 0xC0) != 0x80) return false;
            p += n;
        }
        return true;
    };
    const char *vn = "Đường Nguyễn Trãi";
    bool allValid = true;
    for (size_t cap = 1; cap <= strlen(vn) + 2; cap++) {
        char buf[64]; utf8SafeCopy(buf, cap, vn);
        if (!validUtf8(buf)) { allValid = false; printf("  FAIL: invalid UTF-8 at cap=%zu: '%s'\n", cap, buf); }
        if (strlen(buf) > cap - 1) { allValid = false; printf("  FAIL: overran cap=%zu\n", cap); }
    }
    CHECK(allValid, "utf8SafeCopy never splits a multi-byte char at any cap");

    char full[64]; utf8SafeCopy(full, sizeof(full), vn);
    CHECK(strcmp(full, vn) == 0, "full copy (buffer big enough) is exact");

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
