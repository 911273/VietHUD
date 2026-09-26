#include "DataInstaller.h"
#include "core/SharedState.h" // gnssSnapshot() — refuse to swap data while the vehicle is moving
#include "map/SdCardManager.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>   // esp_random()
#include <esp_task_wdt.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <strings.h>      // strcasecmp()

// Public half of the manifest signing key (private half: ~/.viethud/
// manifest_signing_key.pem on the PC / the Pi pipeline — tools/sign_manifest.py).
// Rotating it = flash a firmware carrying the new key BEFORE publishing data
// signed with it.
static const char kManifestPubKeyPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEln5apjIcBBwLnKzVMOhMFCBVA6rD\n"
    "7FhcGMXJpIKiMDUBgVvLwKBFiOlI4PqASEINgUEu4aTpkTVdW8iZ9B2WzQ==\n"
    "-----END PUBLIC KEY-----\n";

#define LIVE_DIR "/speedmap/"
#define STAGE "/vhupd"
#define LIVE_MANIFEST LIVE_DIR "manifest.txt"
#define LIVE_DATASETS LIVE_DIR "datasets.txt"
#define STAGE_MANIFEST STAGE "/manifest.txt"
#define STAGE_SIG STAGE "/manifest.sig"
#define STAGE_SESSION STAGE "/session.txt"
#define STAGE_DATASETS STAGE "/datasets.new"
#define JOURNAL_TMP STAGE "/commit.tmp"
#define JOURNAL STAGE "/commit.txt"
#define APPLIED STAGE "/applied.txt"
#define LAST_RESULT STAGE "/last.txt"
#define BAK_MANIFEST STAGE "/manifest.bak"
#define BAK_DATASETS STAGE "/datasets.bak"

static const int kMaxFiles = 16;
static const size_t kMaxManifest = 4096;

// ---- data sets: the unit of meaning for the user ("Bản đồ" / "Cảnh báo") ----
// Files are always installed together in one journal, so a data set can never
// be half-updated; the grouping only drives the UI + per-set version record.
struct DatasetDef {
    const char *id;
    const char *label;
    const char *files[6];
};
static const DatasetDef kDatasets[] = {
    {"map", "Bản đồ đường", {"tiles.bin", "index.bin", "metadata.bin", "names.bin", "seg_names.bin", nullptr}},
    {"alerts", "Cảnh báo giao thông", {"signs.bin", "cameras.bin", nullptr}},
};
static const int kDatasetCount = sizeof(kDatasets) / sizeof(kDatasets[0]);

static uint8_t datasetOf(const char *name) {
    for (int d = 0; d < kDatasetCount; d++)
        for (int k = 0; kDatasets[d].files[k]; k++)
            if (strcmp(kDatasets[d].files[k], name) == 0) return (uint8_t)d;
    return 0; // unknown files travel with the map set
}

struct MEntry {
    char name[32];
    uint32_t size;
    char sha[65];
};

struct Session {
    bool active;
    bool restoreTried;
    char sid[33];
    char version[24];
    char manifestSha[65];
    int n;
    MEntry f[kMaxFiles];
    uint32_t recv[kMaxFiles];
    uint8_t ds[kMaxFiles];
};
static Session *S = nullptr; // PSRAM, allocated on first use
static int sWriteIdx = -1;
static const size_t kCoalesce = 32 * 1024;
static uint8_t *sWBuf = nullptr;
static size_t sWLen = 0;
static volatile InstallerProgress sProg = {INST_IDLE, 0, 0, 0, 0, ""};

static bool ensureSession() {
    if (!S) S = (Session *)heap_caps_calloc(1, sizeof(Session), MALLOC_CAP_SPIRAM);
    return S != nullptr;
}

static void setMsg(InstallerState st, const char *msg) {
    sProg.state = st;
    strncpy((char *)sProg.message, msg ? msg : "", sizeof(sProg.message) - 1);
    ((char *)sProg.message)[sizeof(sProg.message) - 1] = '\0';
    Serial.printf("[install] %s\n", msg ? msg : "");
}

static void wdtTick() { esp_task_wdt_reset(); }

static void hexEncode(const uint8_t *in, int len, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static void sha256Hex(const void *data, size_t len, char out[65]) {
    uint8_t h[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const uint8_t *)data, len);
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    hexEncode(h, 32, out);
}

// Only plain file names ever reach the card: no '/', no leading '.', bounded.
static bool safeName(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 31 || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return false;
    }
    return strcmp(s, "manifest.txt") != 0 && strcmp(s, "datasets.txt") != 0;
}

static bool safeVersion(const char *s) {
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '.' ||
              *s == '-' || *s == '_'))
            return false;
    return true;
}

// "version <v>" + "<name> <size> <sha256>" lines (the format the pipeline and
// the old DataUpdater already share). Returns entries parsed, -1 on a malformed
// or unsafe line (a signed manifest should never have one — refuse outright).
static int parseManifest(const char *text, size_t len, MEntry *out, int maxN, char *ver, size_t verCap) {
    int n = 0;
    if (ver && verCap) ver[0] = '\0';
    size_t p = 0;
    while (p < len) {
        char line[160];
        size_t i = 0;
        while (p < len && text[p] != '\n' && i < sizeof(line) - 1) line[i++] = text[p++];
        while (p < len && text[p] != '\n') p++; // overlong line: truncated, then rejected below
        if (p < len) p++;
        line[i] = '\0';
        if (i && line[i - 1] == '\r') line[--i] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        char a[48], b[24], c[80];
        int k = sscanf(line, "%47s %23s %79s", a, b, c);
        if (k >= 2 && strcmp(a, "version") == 0) {
            if (ver && verCap && safeVersion(b)) {
                strncpy(ver, b, verCap - 1);
                ver[verCap - 1] = '\0';
            }
            continue;
        }
        if (k != 3 || !safeName(a) || strlen(c) != 64 || n >= maxN) return -1;
        strncpy(out[n].name, a, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        out[n].size = (uint32_t)strtoul(b, nullptr, 10);
        strncpy(out[n].sha, c, 64);
        out[n].sha[64] = '\0';
        n++;
    }
    return n;
}

// Reads a small text file (manifest/journal) into buf (NUL-terminated). -1 if absent.
static int readSmall(const char *path, char *buf, size_t cap) {
    int got = sdMgrReadFileChunk(path, 0, (uint8_t *)buf, cap - 1);
    if (got < 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[got] = '\0';
    return got;
}

static void partPath(const char *name, const char *suffix, char *out, size_t cap) {
    snprintf(out, cap, STAGE "/%s%s", name, suffix);
}

bool installerVerifySignature(const char *text, size_t len, const char *sigB64) {
    if (!text || !sigB64) return false;
    char clean[160];
    size_t c = 0;
    for (const char *p = sigB64; *p && c < sizeof(clean) - 1; p++)
        if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') clean[c++] = *p;
    clean[c] = '\0';
    unsigned char sig[96];
    size_t slen = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &slen, (const unsigned char *)clean, c) != 0 || slen < 8) return false;
    uint8_t hash[32];
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    mbedtls_sha256_update(&h, (const uint8_t *)text, len);
    mbedtls_sha256_finish(&h, hash);
    mbedtls_sha256_free(&h);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int r = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)kManifestPubKeyPem, sizeof(kManifestPubKeyPem));
    if (r == 0) r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, slen);
    mbedtls_pk_free(&pk);
    if (r != 0) Serial.printf("[install] signature check failed (-0x%04x)\n", (unsigned)-r);
    return r == 0;
}

// Local (installed) manifest entries.
static int localEntries(MEntry *out, int maxN, char *ver, size_t verCap) {
    char *buf = (char *)heap_caps_malloc(kMaxManifest + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return 0;
    int got = readSmall(LIVE_MANIFEST, buf, kMaxManifest + 1);
    int n = got > 0 ? parseManifest(buf, got, out, maxN, ver, verCap) : 0;
    heap_caps_free(buf);
    if (n < 0) n = 0;
    return n;
}

static void readDatasetVersions(char vers[][24]); // defined below (local version record)

static void updateProgressTotals() {
    uint32_t done = 0, total = 0;
    uint8_t filesDone = 0;
    for (int i = 0; i < S->n; i++) {
        done += S->recv[i];
        total += S->f[i].size;
        if (S->recv[i] >= S->f[i].size) filesDone++;
    }
    sProg.bytesDone = done;
    sProg.bytesTotal = total;
    sProg.filesDone = filesDone;
    sProg.filesTotal = (uint8_t)S->n;
}

static bool allReceived() {
    for (int i = 0; i < S->n; i++)
        if (S->recv[i] < S->f[i].size) return false;
    return true;
}

// Build S (needed files + received sizes) from a verified remote manifest.
static bool loadNeeded(const char *text, size_t len) {
    MEntry *remote = (MEntry *)heap_caps_malloc(sizeof(MEntry) * kMaxFiles * 2, MALLOC_CAP_SPIRAM);
    if (!remote) return false;
    MEntry *local = remote + kMaxFiles;
    int rn = parseManifest(text, len, remote, kMaxFiles, S->version, sizeof(S->version));
    int ln = localEntries(local, kMaxFiles, nullptr, 0);
    S->n = 0;
    for (int i = 0; rn > 0 && i < rn; i++) {
        bool same = false;
        for (int j = 0; j < ln; j++)
            if (strcmp(local[j].name, remote[i].name) == 0 && strcasecmp(local[j].sha, remote[i].sha) == 0) same = true;
        if (same) continue;
        S->f[S->n] = remote[i];
        S->ds[S->n] = datasetOf(remote[i].name);
        char pp[64];
        partPath(remote[i].name, ".part", pp, sizeof(pp));
        int64_t sz = sdMgrFileSize(pp);
        S->recv[S->n] = sz > 0 ? (uint32_t)(sz > remote[i].size ? remote[i].size : sz) : 0;
        S->n++;
    }
    heap_caps_free(remote);
    return rn > 0;
}

// After a reboot mid-upload: pick the session back up from the card.
static void restoreSession() {
    if (!ensureSession() || S->restoreTried) return;
    S->restoreTried = true;
    if (!sdMgrExists(STAGE_SESSION) || sdMgrExists(JOURNAL)) return;
    char sidBuf[80];
    if (readSmall(STAGE_SESSION, sidBuf, sizeof(sidBuf)) <= 0) return;
    char *text = (char *)heap_caps_malloc(kMaxManifest + 1, MALLOC_CAP_SPIRAM);
    if (!text) return;
    int len = readSmall(STAGE_MANIFEST, text, kMaxManifest + 1);
    if (len > 0 && sscanf(sidBuf, "sid %32s", S->sid) == 1 && loadNeeded(text, len) && S->n > 0) {
        sha256Hex(text, len, S->manifestSha);
        S->active = true;
        updateProgressTotals();
        setMsg(allReceived() ? INST_READY : INST_RECEIVING, "Tiếp tục phiên cập nhật dở");
    }
    heap_caps_free(text);
}

int installerOpenSession(const char *text, size_t len, const char *sigB64, char *err, size_t errCap) {
    auto fail = [&](int code, const char *m) {
        snprintf(err, errCap, "%s", m);
        return code;
    };
    if (!ensureSession()) return fail(503, "Hết bộ nhớ");
    if (!sdMgrMkdir(STAGE)) return fail(503, "Không đọc được thẻ nhớ");
    restoreSession();
    if (sdMgrExists(JOURNAL)) return fail(409, "Đang chờ khởi động lại để cài bản trước");
    if (sdMgrExists(APPLIED)) return fail(409, "Đang xác nhận bản vừa cài — thử lại sau 1 phút");
    if (!text || len == 0 || len > kMaxManifest) return fail(400, "Manifest không hợp lệ");
    if (!installerVerifySignature(text, len, sigB64)) return fail(401, "Chữ ký dữ liệu không hợp lệ");

    char mSha[65];
    sha256Hex(text, len, mSha);
    if (S->active && strcmp(S->manifestSha, mSha) == 0) { // same release: resume
        for (int i = 0; i < S->n; i++) {
            char pp[64];
            partPath(S->f[i].name, ".part", pp, sizeof(pp));
            int64_t sz = sdMgrFileSize(pp);
            S->recv[i] = sz > 0 ? (uint32_t)sz : 0;
        }
        updateProgressTotals();
        setMsg(allReceived() ? INST_READY : INST_RECEIVING, "Tiếp tục nhận dữ liệu");
        return 201;
    }

    // New release: start clean (parts of an older release are useless now).
    sdMgrWriterClose();
    sWriteIdx = -1;
    sWLen = 0;
    sdMgrClearDir(STAGE);
    memset(S, 0, sizeof(*S));
    S->restoreTried = true;
    if (!loadNeeded(text, len)) return fail(400, "Manifest không hợp lệ");
    if (S->n == 0) {
        // Every file already matches, but the (signed) manifest itself may be a
        // newer release (new version line, or entries we already had). Adopt it
        // so version displays / the online auto-check stop reporting an update.
        // tmp + rename: a power cut leaves either the old or the new manifest.
        char *cur = (char *)heap_caps_malloc(kMaxManifest + 1, MALLOC_CAP_SPIRAM);
        int cl = cur ? readSmall(LIVE_MANIFEST, cur, kMaxManifest + 1) : -1;
        char vers[kDatasetCount][24];
        readDatasetVersions(vers);
        bool versStale = false;
        for (int d = 0; d < kDatasetCount; d++) versStale |= S->version[0] && strcmp(vers[d], S->version) != 0;
        bool textStale = cur && (cl != (int)len || memcmp(cur, text, len) != 0);
        if (textStale && sdMgrWriteSmallFile(LIVE_DIR "manifest.tmp", text, len)) {
            sdMgrRemove(LIVE_MANIFEST);
            sdMgrMove(LIVE_DIR "manifest.tmp", LIVE_MANIFEST);
            Serial.printf("[install] adopted manifest %s (no data changes)\n", S->version);
        }
        if ((textStale || versStale) && S->version[0]) {
            char dsBuf[160];
            int dl = 0;
            for (int d = 0; d < kDatasetCount; d++)
                dl += snprintf(dsBuf + dl, sizeof(dsBuf) - dl, "%s %s\n", kDatasets[d].id, S->version);
            sdMgrWriteSmallFile(LIVE_DATASETS, dsBuf, dl);
        }
        if (cur) heap_caps_free(cur);
        S->active = false;
        setMsg(INST_IDLE, "Dữ liệu đã là bản mới nhất");
        return fail(200, "Dữ liệu đã là bản mới nhất");
    }
    uint64_t need = 512 * 1024;
    for (int i = 0; i < S->n; i++) need += S->f[i].size;
    uint64_t freeB = sdMgrFreeBytes();
    if (freeB < need) {
        snprintf(err, errCap, "Thẻ nhớ đầy: cần %u MB, còn %u MB", (unsigned)(need >> 20), (unsigned)(freeB >> 20));
        return 507;
    }
    uint8_t rnd[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(rnd + i, &r, 4);
    }
    hexEncode(rnd, 16, S->sid);
    strncpy(S->manifestSha, mSha, sizeof(S->manifestSha));
    char sidLine[48];
    int sl = snprintf(sidLine, sizeof(sidLine), "sid %s\n", S->sid);
    if (!sdMgrWriteSmallFile(STAGE_MANIFEST, text, len) || !sdMgrWriteSmallFile(STAGE_SIG, sigB64, strlen(sigB64)) ||
        !sdMgrWriteSmallFile(STAGE_SESSION, sidLine, sl))
        return fail(503, "Không ghi được thẻ nhớ");
    S->active = true;
    updateProgressTotals();
    char m[72];
    snprintf(m, sizeof(m), "Chờ nhận %d tệp (%u KB)", S->n, (unsigned)(sProg.bytesTotal / 1024));
    setMsg(INST_RECEIVING, m);
    return 201;
}

bool installerSessionActive() {
    restoreSession();
    return S && S->active;
}
const char *installerSessionId() { return (S && S->active) ? S->sid : ""; }
int installerFileCount() { return (S && S->active) ? S->n : 0; }
bool installerFileInfo(int i, const char **name, uint32_t *size, uint32_t *received, uint8_t *ds) {
    if (!S || !S->active || i < 0 || i >= S->n) return false;
    if (name) *name = S->f[i].name;
    if (size) *size = S->f[i].size;
    if (received) *received = S->recv[i];
    if (ds) *ds = S->ds[i];
    return true;
}

int installerWriteBegin(const char *name, uint32_t offset, uint32_t *expected) {
    restoreSession();
    if (!S || !S->active) return 410;
    int idx = -1;
    for (int i = 0; i < S->n; i++)
        if (strcmp(S->f[i].name, name) == 0) idx = i;
    if (idx < 0) return 404;
    char pp[64];
    partPath(S->f[idx].name, ".part", pp, sizeof(pp));
    int64_t cur = sdMgrFileSize(pp);
    if (cur < 0) cur = 0;
    S->recv[idx] = (uint32_t)cur;
    if ((uint32_t)cur != offset) {
        if (expected) *expected = (uint32_t)cur;
        return 409;
    }
    if (!sdMgrWriterOpen(pp, true)) return 503;
    sWriteIdx = idx;
    if (sProg.state != INST_RECEIVING) setMsg(INST_RECEIVING, "Đang nhận dữ liệu");
    return 200;
}

// Incoming bytes arrive in ~1.4 KB pieces (WebServer raw buffer / TLS reads).
// Writing those straight to FAT costs a sector read-modify-write each time
// (measured: ~20 KB/s end-to-end), so they are coalesced in a 32 KB PSRAM
// buffer and written in big blocks. Anything still buffered at a power cut is
// simply lost — the part's REAL size on the card is what resume uses.

static bool flushCoalesced() {
    if (sWLen == 0) return true;
    bool ok = sdMgrWriterWrite(sWBuf, sWLen);
    sWLen = 0;
    return ok;
}

bool installerWriteData(const uint8_t *buf, size_t len) {
    if (sWriteIdx < 0 || !S) return false;
    uint32_t room = S->f[sWriteIdx].size - S->recv[sWriteIdx];
    if (len > room) return false; // never grow a part past its manifest size
    if (!sWBuf) sWBuf = (uint8_t *)heap_caps_malloc(kCoalesce, MALLOC_CAP_SPIRAM);
    if (!sWBuf) {
        if (!sdMgrWriterWrite(buf, len)) return false;
    } else {
        while (len) {
            size_t n = kCoalesce - sWLen;
            if (n > len) n = len;
            memcpy(sWBuf + sWLen, buf, n);
            sWLen += n;
            buf += n;
            len -= n;
            S->recv[sWriteIdx] += n;
            sProg.bytesDone += n;
            if (sWLen == kCoalesce && !flushCoalesced()) return false;
        }
        return true;
    }
    S->recv[sWriteIdx] += len;
    sProg.bytesDone += len;
    return true;
}

uint32_t installerWriteEnd() {
    if (sWriteIdx < 0 || !S) return 0;
    flushCoalesced();
    sdMgrWriterClose();
    char pp[64];
    partPath(S->f[sWriteIdx].name, ".part", pp, sizeof(pp));
    int64_t sz = sdMgrFileSize(pp);
    S->recv[sWriteIdx] = sz > 0 ? (uint32_t)sz : 0;
    uint32_t r = S->recv[sWriteIdx];
    sWriteIdx = -1;
    updateProgressTotals();
    if (allReceived()) setMsg(INST_READY, "Đã nhận đủ dữ liệu");
    return r;
}

void installerAbort() {
    sdMgrWriterClose();
    sWriteIdx = -1;
    sWLen = 0;
    if (!sdMgrExists(JOURNAL) && !sdMgrExists(APPLIED)) sdMgrClearDir(STAGE);
    if (S) {
        memset(S, 0, sizeof(*S));
        S->restoreTried = true;
    }
    sProg.bytesDone = sProg.bytesTotal = 0;
    sProg.filesDone = sProg.filesTotal = 0;
    setMsg(INST_IDLE, "Đã hủy cập nhật");
}

static void readDatasetVersions(char vers[][24]) {
    char local[24];
    installerLocalVersion(local, sizeof(local));
    for (int d = 0; d < kDatasetCount; d++) strncpy(vers[d], local, 23), vers[d][23] = '\0';
    char buf[256];
    if (readSmall(LIVE_DATASETS, buf, sizeof(buf)) <= 0) return;
    char *save = nullptr;
    for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(nullptr, "\n", &save)) {
        char id[16], v[24];
        if (sscanf(ln, "%15s %23s", id, v) != 2 || !safeVersion(v)) continue;
        for (int d = 0; d < kDatasetCount; d++)
            if (strcmp(id, kDatasets[d].id) == 0) strcpy(vers[d], v);
    }
}

int installerCommit(bool checkMoving, char *err, size_t errCap) {
    auto fail = [&](int code, const char *m) {
        snprintf(err, errCap, "%s", m);
        return code;
    };
    restoreSession();
    if (!S || !S->active) return fail(410, "Không có phiên cập nhật");
    if (!allReceived()) return fail(412, "Chưa nhận đủ dữ liệu");
    if (checkMoving) {
        GnssSnapshot g = gnssSnapshot();
        if (g.fix && g.egoSpeedKmh > 8.0f) return fail(423, "Xe đang chạy — dừng xe để cài đặt");
    }
    setMsg(INST_VERIFYING, "Đang kiểm tra dữ liệu (SHA-256)...");
    for (int i = 0; i < S->n; i++) {
        char pp[64];
        partPath(S->f[i].name, ".part", pp, sizeof(pp));
        int64_t sz = sdMgrFileSize(pp);
        uint8_t h[32];
        char got[65] = "";
        bool shaOk = sz == (int64_t)S->f[i].size && sdMgrSha256File(pp, h, wdtTick);
        if (shaOk) {
            hexEncode(h, 32, got);
            shaOk = strcasecmp(got, S->f[i].sha) == 0;
        }
        if (!shaOk) {
            sdMgrRemove(pp); // corrupt part: drop it so the client re-sends just this file
            S->recv[i] = 0;
            updateProgressTotals();
            char m[72];
            snprintf(m, sizeof(m), "Tệp %s bị lỗi khi truyền — gửi lại", S->f[i].name);
            setMsg(INST_FAILED, m);
            return fail(422, m);
        }
    }
    // Stage: .part -> .new (verified), then the per-set version record.
    for (int i = 0; i < S->n; i++) {
        char pp[64], np[64];
        partPath(S->f[i].name, ".part", pp, sizeof(pp));
        partPath(S->f[i].name, ".new", np, sizeof(np));
        sdMgrRemove(np);
        if (!sdMgrMove(pp, np)) {
            setMsg(INST_FAILED, "Lỗi ghi thẻ nhớ");
            return fail(503, "Lỗi ghi thẻ nhớ");
        }
    }
    // After this install EVERY file matches the new signed manifest (untouched
    // sets were already identical to it), so every data set is "at" its version.
    char dsBuf[160];
    int dl = 0;
    for (int d = 0; d < kDatasetCount; d++)
        dl += snprintf(dsBuf + dl, sizeof(dsBuf) - dl, "%s %s\n", kDatasets[d].id, S->version);
    sdMgrWriteSmallFile(STAGE_DATASETS, dsBuf, dl);
    // Journal: write to a temp name, then rename = the atomic commit point.
    char jb[kMaxFiles * 34 + 1];
    int jl = 0;
    for (int i = 0; i < S->n; i++) jl += snprintf(jb + jl, sizeof(jb) - jl, "%s\n", S->f[i].name);
    sdMgrRemove(JOURNAL_TMP);
    if (!sdMgrWriteSmallFile(JOURNAL_TMP, jb, jl) || !sdMgrMove(JOURNAL_TMP, JOURNAL)) {
        setMsg(INST_FAILED, "Lỗi ghi nhật ký cài đặt");
        return fail(503, "Lỗi ghi nhật ký cài đặt");
    }
    sdMgrRemove(STAGE_SESSION);
    S->active = false;
    Preferences p;
    p.begin("vhupd", false);
    p.putBool("wifiOn", true); // bring the AP back after the reboot so the phone sees the result
    p.end();
    setMsg(INST_COMMITTED, "Dữ liệu hợp lệ — khởi động lại để cài đặt");
    return 200;
}

InstallerProgress installerProgress() {
    InstallerProgress p;
    memcpy(&p, (const void *)&sProg, sizeof(p));
    return p;
}

// ---- boot-time apply / rollback ------------------------------------------

// One journal step, idempotent: safe to re-run after a power cut at ANY point.
static void applyOne(const char *live, const char *nw, const char *bak) {
    if (!sdMgrExists(nw)) return; // already moved in on an earlier (interrupted) run
    if (sdMgrExists(live)) {
        sdMgrRemove(bak);
        sdMgrMove(live, bak);
    }
    sdMgrMove(nw, live);
}

static void restoreOne(const char *live, const char *bak) {
    if (!sdMgrExists(bak)) return;
    sdMgrRemove(live);
    sdMgrMove(bak, live);
}

static void writeLast(const char *kind, const char *detail) {
    char b[96];
    int l = snprintf(b, sizeof(b), "%s %s\n", kind, detail);
    sdMgrWriteSmallFile(LAST_RESULT, b, l);
}

static int bootCounter(int delta, bool reset) {
    Preferences p;
    p.begin("vhupd", false);
    int v = reset ? 0 : p.getInt("boots", 0) + delta;
    p.putInt("boots", v);
    p.end();
    return v;
}

static void forEachJournalName(const char *path, void (*fn)(const char *name)) {
    char buf[kMaxFiles * 34 + 1];
    if (readSmall(path, buf, sizeof(buf)) <= 0) return;
    char *save = nullptr;
    for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(nullptr, "\n", &save)) {
        size_t l = strlen(ln);
        if (l && ln[l - 1] == '\r') ln[l - 1] = '\0';
        if (safeName(ln)) fn(ln);
    }
}

static void rollback(const char *reason) {
    Serial.printf("[install] ROLLBACK: %s\n", reason);
    forEachJournalName(APPLIED, [](const char *n) {
        char live[64], bak[64];
        snprintf(live, sizeof(live), LIVE_DIR "%s", n);
        partPath(n, ".bak", bak, sizeof(bak));
        restoreOne(live, bak);
    });
    restoreOne(LIVE_MANIFEST, BAK_MANIFEST);
    restoreOne(LIVE_DATASETS, BAK_DATASETS);
    sdMgrRemove(APPLIED);
    writeLast("rollback", reason);
    bootCounter(0, true);
}

static bool sRolledBackThisBoot = false;

bool installerBootApply() {
    {   // a rollback that rebooted (installerAfterMount) asks this boot to say so on screen
        Preferences p;
        p.begin("vhupd", false);
        if (p.getBool("rbShow", false)) {
            sRolledBackThisBoot = true;
            p.putBool("rbShow", false);
        }
        p.end();
    }
    if (!sdMgrExists(JOURNAL)) {
        if (sdMgrExists(APPLIED)) {
            // A fresh install that hasn't yet proven itself (confirmHealthy).
            int boots = bootCounter(1, false);
            Serial.printf("[install] unconfirmed install, boot #%d\n", boots);
            if (boots >= 3) {
                rollback("Khởi động lỗi nhiều lần");
                sRolledBackThisBoot = true;
            }
        }
        return false;
    }
    Serial.println("[install] applying staged data (journal found)");
    forEachJournalName(JOURNAL, [](const char *n) {
        char live[64], nw[64], bak[64];
        snprintf(live, sizeof(live), LIVE_DIR "%s", n);
        partPath(n, ".new", nw, sizeof(nw));
        partPath(n, ".bak", bak, sizeof(bak));
        applyOne(live, nw, bak);
        esp_task_wdt_reset();
    });
    applyOne(LIVE_MANIFEST, STAGE_MANIFEST, BAK_MANIFEST);
    applyOne(LIVE_DATASETS, STAGE_DATASETS, BAK_DATASETS);
    // Journal -> "applied, awaiting confirmation" (keeps the list for rollback).
    char buf[kMaxFiles * 34 + 1];
    int l = readSmall(JOURNAL, buf, sizeof(buf));
    if (l > 0) sdMgrWriteSmallFile(APPLIED, buf, l);
    sdMgrRemove(JOURNAL);
    sdMgrRemove(STAGE_SIG);
    sdMgrRemove(STAGE_SESSION);
    char ver[24];
    installerLocalVersion(ver, sizeof(ver));
    writeLast("ok", ver);
    bootCounter(1, true);
    bootCounter(1, false);
    Serial.printf("[install] applied data version %s\n", ver);
    return true;
}

void installerAfterMount(bool mountOk) {
    if (mountOk || !sdMgrExists(APPLIED)) return;
    rollback("Dữ liệu mới không đọc được");
    Preferences p;
    p.begin("vhupd", false);
    p.putBool("rbShow", true); // show "KHÔI PHỤC DỮ LIỆU CŨ" after the restart below
    p.end();
    delay(300);
    ESP.restart();
}

void installerConfirmHealthy() {
    if (!sdMgrExists(APPLIED)) return;
    forEachJournalName(APPLIED, [](const char *n) {
        char bak[64];
        partPath(n, ".bak", bak, sizeof(bak));
        sdMgrRemove(bak);
    });
    sdMgrRemove(BAK_MANIFEST);
    sdMgrRemove(BAK_DATASETS);
    sdMgrRemove(APPLIED);
    bootCounter(0, true);
    Serial.println("[install] new data confirmed healthy — backups removed");
}

InstallerLastResult installerLastResult(char *detail, size_t cap, bool clear) {
    char buf[96];
    if (cap) detail[0] = '\0';
    if (readSmall(LAST_RESULT, buf, sizeof(buf)) <= 0) return INST_LAST_NONE;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    InstallerLastResult r = INST_LAST_NONE;
    const char *rest = "";
    if (strncmp(buf, "ok ", 3) == 0) r = INST_LAST_OK, rest = buf + 3;
    else if (strncmp(buf, "rollback ", 9) == 0) r = INST_LAST_ROLLED_BACK, rest = buf + 9;
    if (cap) {
        strncpy(detail, rest, cap - 1);
        detail[cap - 1] = '\0';
    }
    if (clear) sdMgrRemove(LAST_RESULT);
    return r;
}

bool installerRolledBackThisBoot() { return sRolledBackThisBoot; }

bool installerTakeWifiOnRequest() {
    Preferences p;
    p.begin("vhupd", false);
    bool on = p.getBool("wifiOn", false);
    if (on) p.putBool("wifiOn", false);
    p.end();
    return on;
}

// ---- local state ----------------------------------------------------------

void installerLocalVersion(char *out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    char buf[128];
    if (readSmall(LIVE_MANIFEST, buf, sizeof(buf)) <= 0) return;
    char v[24];
    if (sscanf(buf, "version %23s", v) == 1 && safeVersion(v)) {
        strncpy(out, v, cap - 1);
        out[cap - 1] = '\0';
    }
}

int installerDatasetCount() { return kDatasetCount; }
const char *installerDatasetId(int i) { return (i >= 0 && i < kDatasetCount) ? kDatasets[i].id : ""; }
const char *installerDatasetLabel(int i) { return (i >= 0 && i < kDatasetCount) ? kDatasets[i].label : ""; }
void installerDatasetVersion(int i, char *out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    if (i < 0 || i >= kDatasetCount) return;
    char vers[kDatasetCount][24];
    readDatasetVersions(vers);
    strncpy(out, vers[i], cap - 1);
    out[cap - 1] = '\0';
}

static const char *stateName(InstallerState s) {
    switch (s) {
    case INST_RECEIVING: return "receiving";
    case INST_READY: return "ready";
    case INST_VERIFYING: return "verifying";
    case INST_COMMITTED: return "committed";
    case INST_FAILED: return "failed";
    default: return "idle";
    }
}

void installerSessionJson(String &out) {
    restoreSession();
    if (!S || !S->active) {
        out += "null";
        return;
    }
    char b[160];
    snprintf(b, sizeof(b), "{\"sid\":\"%s\",\"state\":\"%s\",\"version\":\"%s\",\"bytesDone\":%u,\"bytesTotal\":%u,\"files\":[",
             S->sid, stateName(sProg.state), S->version, (unsigned)sProg.bytesDone, (unsigned)sProg.bytesTotal);
    out += b;
    for (int i = 0; i < S->n; i++) {
        snprintf(b, sizeof(b), "%s{\"n\":\"%s\",\"s\":%u,\"r\":%u,\"d\":\"%s\"}", i ? "," : "", S->f[i].name,
                 (unsigned)S->f[i].size, (unsigned)S->recv[i], kDatasets[S->ds[i]].id);
        out += b;
    }
    out += "]}";
}

void installerStateJson(String &out) {
    static uint64_t sFree = 0;
    static uint32_t sFreeAt = 0;
    if (sFreeAt == 0 || millis() - sFreeAt > 30000) {
        sFree = sdMgrFreeBytes();
        sFreeAt = millis() | 1;
    }
    MEntry *local = (MEntry *)heap_caps_malloc(sizeof(MEntry) * kMaxFiles, MALLOC_CAP_SPIRAM);
    char ver[24] = "";
    int ln = local ? localEntries(local, kMaxFiles, ver, sizeof(ver)) : 0;
    char b[200];
    snprintf(b, sizeof(b), "{\"sd\":{\"ok\":%s,\"freeMB\":%u},\"local\":{\"version\":\"%s\",\"files\":[",
             sdMgrExists("/") ? "true" : "false", (unsigned)(sFree >> 20), ver);
    out += b;
    for (int i = 0; i < ln; i++) {
        snprintf(b, sizeof(b), "%s{\"n\":\"%s\",\"s\":%u,\"h\":\"%s\"}", i ? "," : "", local[i].name,
                 (unsigned)local[i].size, local[i].sha);
        out += b;
    }
    if (local) heap_caps_free(local);
    out += "]},\"datasets\":[";
    char vers[kDatasetCount][24];
    readDatasetVersions(vers);
    for (int d = 0; d < kDatasetCount; d++) {
        snprintf(b, sizeof(b), "%s{\"id\":\"%s\",\"label\":\"%s\",\"version\":\"%s\",\"files\":[", d ? "," : "",
                 kDatasets[d].id, kDatasets[d].label, vers[d]);
        out += b;
        for (int k = 0; kDatasets[d].files[k]; k++) {
            out += k ? ",\"" : "\"";
            out += kDatasets[d].files[k];
            out += "\"";
        }
        out += "]}";
    }
    char last[64];
    InstallerLastResult lr = installerLastResult(last, sizeof(last), false);
    snprintf(b, sizeof(b), "],\"last\":{\"result\":\"%s\",\"detail\":\"%s\"},\"pendingReboot\":%s,\"session\":",
             lr == INST_LAST_OK ? "ok" : lr == INST_LAST_ROLLED_BACK ? "rollback" : "none", last,
             sdMgrExists(JOURNAL) ? "true" : "false");
    out += b;
    installerSessionJson(out);
    snprintf(b, sizeof(b), ",\"progress\":{\"state\":\"%s\",\"msg\":\"%s\"}}", stateName(sProg.state),
             (const char *)sProg.message);
    out += b;
}
