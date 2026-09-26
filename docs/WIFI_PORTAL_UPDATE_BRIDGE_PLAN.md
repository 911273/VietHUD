# VietHUD — Kế hoạch Wi-Fi Provisioning + Web Portal + Phone Update Bridge

> Trạng thái: **KẾ HOẠCH — chưa code.** Lập ngày 2026-09-26 dựa trên source thực tế
> của `radar_car` (branch `main`, có 8 file đang sửa chưa commit: `WebPortal.cpp`,
> `DataUpdater.cpp`, `Settings.cpp`, `AppConfig.h`, `NvsStore.cpp`, `lv_conf.h`, …).
> Toolchain: `espressif32@7.1.3` = arduino-esp32 **2.0.17 / ESP-IDF 4.4**,
> board ESP32-S3 N16R8, partition `default_16MB.csv`.

Ký hiệu độ tin cậy dùng trong tài liệu:

- **[ĐÃ KIỂM]** — đã kiểm chứng trong buổi lập kế hoạch này (đọc code / curl / đọc header SDK).
- **[CẦN ĐO]** — hành vi của hệ điều hành / trình duyệt mà tài liệu công khai không đủ chắc chắn; **phải đo trên điện thoại thật ở Phase 0** trước khi chốt thiết kế.

---

## 0. Tóm tắt điều hành — những gì đọc code phát hiện ra

Nền móng đã có khá nhiều (AP, captive portal, QR, WebServer, Wi-Fi Manager, DataUpdater có SHA-256 + file `.tmp`). Nhưng có **8 điểm sẽ gây conflict/crash hoặc làm Phone Bridge thất bại** nếu chỉ "thêm endpoint":

| # | Phát hiện | Hậu quả nếu bỏ qua | Vị trí |
|---|-----------|--------------------|--------|
| C1 | **Captive DNS trả mọi tên miền về 192.168.4.1** (`dnsServer.start(53,"*",kApIp)`) và DHCP có quảng bá gateway | Nếu điện thoại dùng DNS của Wi-Fi VietHUD thì `raw.githubusercontent.com` bị phân giải về ESP32 → Phone Bridge **chắc chắn hỏng** | `WebPortal.cpp:1011-1012` |
| C2 | **`wmTick()` quét Wi-Fi mỗi 15 s** khi có mạng đã lưu nhưng không trong tầm (`WiFi.disconnect` + `scanNetworks` đồng bộ 2–4 s) | AP nhảy kênh / nghẽn → phiên upload từ điện thoại bị rớt giữa chừng | `WebPortal.cpp:900-965` |
| C3 | Cài đặt hiện tại **đổi tên từng file một** (`remove(to)` rồi `rename`) | Mất điện giữa chừng → `tiles.bin` mới + `names.bin` cũ (lệch bộ) hoặc **thiếu file** → map hỏng | `DataUpdater.cpp:196-203`, `SdCardManager.cpp sdMgrRename` |
| C4 | `index/cameras/signs/names` được **nạp vào PSRAM lúc boot**, còn `tiles.bin` được mở lại mỗi lần đọc | Thay `tiles.bin` khi đang chạy → offset trong index (PSRAM) trỏ sai vào file mới → segment rác | `SdCardManager.cpp:229-420, 476` |
| C5 | `sdMgrAppendBytes` **open-append-close mỗi chunk** | Trên FAT, mỗi lần mở-append phải duyệt chuỗi cluster → file 5 MB ghi chậm dần (gần O(n²)); upload qua Wi-Fi sẽ rất chậm | `SdCardManager.cpp:686` |
| C6 | `webTask` đăng ký **task watchdog** (10 s, panic=true) và WebServer là đồng bộ | Một request upload lớn chạy trong `handleClient()` > 10 s → **WDT reset** | `WebPortal.cpp:1057`, `main_viethud.cpp:325` |
| C7 | **GitHub Release asset KHÔNG có CORS** — [ĐÃ KIỂM] `release-assets.githubusercontent.com` không trả `Access-Control-Allow-Origin`; còn `raw.githubusercontent.com` trả `*` | Trang ở `http://192.168.4.1` **không thể `fetch()` file từ GitHub Release**; chỉ tải được từ raw/Pages/jsDelivr | curl 2026-09-26 |
| C8 | Trang portal là `http://` (không phải secure context) | **Không có `crypto.subtle`**, không Service Worker, không Cache API, không OPFS → không PWA, SHA-256 phía điện thoại phải dùng JS thuần | chuẩn Web |

Thêm các lỗ hổng bảo mật hiện hữu: mật khẩu AP mặc định **`12345678` giống nhau trên mọi máy**, portal **không có xác thực**, `/update` nhận firmware **không kiểm chữ ký/checksum** — bất kỳ ai vào được AP đều flash được firmware.

**Kết luận thiết kế chính:**

1. Phone Bridge **không thể dựa vào một cơ chế duy nhất**. Thiết kế 3 tầng vận chuyển, tự động chọn:
   - **B1 — Song song (dual-network):** điện thoại vừa nối AP VietHUD vừa dùng 4G. Tốt nhất nhưng phụ thuộc OS **[CẦN ĐO]**.
   - **B2 — Tuần tự trong cùng tab:** trang portal tải dữ liệu lúc điện thoại đang ở 4G (tạm rời Wi-Fi VietHUD), giữ trong IndexedDB, nối lại rồi đẩy xuống. Chạy trên **mọi** trình duyệt.
   - **B3 — File thủ công:** tải gói `.vhpkg` từ GitHub Release bằng trình duyệt thường, rồi chọn file trong portal. Chắc chắn chạy, kể cả từ laptop.
2. **ESP32 là cửa kiểm soát duy nhất về an toàn**: manifest được **ký ECDSA P-256** (khóa riêng nằm trên pipeline Pi, khóa công khai nhúng firmware). Điện thoại kiểm SHA để UX tốt/không upload rác, nhưng thiết bị không tin điện thoại.
3. **Mọi con đường (A, B1, B2, B3) dùng chung một pipeline trên thiết bị:** staging → verify → commit journal → **cài lúc boot trước `sdMgrMount()`** → tự rollback. Sửa luôn C3/C4 cho Mode A hiện tại.
4. Thêm **"AP profile Bridge"**: DHCP không quảng bá gateway/DNS (cờ `OFFER_ROUTER`/`OFFER_DNS` có sẵn trong dhcpserver của IDF 4.4 — [ĐÃ KIỂM] `dhcpserver.h:52-53`), DNS chỉ trả tên nội bộ. Đây là đòn bẩy chính để OS giữ 4G làm đường Internet mặc định.

---

## 1. Kiến trúc hiện tại (Current architecture)

```text
Core 1  loopTask (main_viethud.cpp)  — LVGL UI: Dashboard / Settings, WDT 10s
Core 0  gnssTask(3K, prio2) touchTask(2.5K, prio3) audioTask(6K)
        speedLimitTask(16K)  — map matcher + mapRendererUpdate + đọc tile SD
        tripLog(5K)  webTask(8K)  [dataUpd(8K) / dataChk(12K) — tạo khi cần]
SD      SD_MMC 1-bit, 1 mutex (SdLock) cho mọi truy cập — /speedmap/*, /triplog/*
PSRAM   LVGL pool 512K, index/signs(~1.9MB)/cameras/names nạp lúc sdMgrMount()
NVS     AppConfig (Preferences), ns "dataupd" cờ pending
Flash   app0/app1 6.25MB mỗi bên (OTA 2 slot), spiffs 3.4MB (không dùng), coredump
```

Chuỗi boot (`setup()`): WDT → touch → jingle → LVGL/display (draw buffer 8 dòng, internal RAM) → `sdMgrMount()` → **nếu cờ `dataupd/pending` → `runDataUpdateMode()` (không bao giờ return)** → `buildDashboard()` → `gnssTaskStart()` → `webPortalInit()` → `speedLimitManagerStart()`.

Nguyên tắc sẵn có cần giữ: tất cả lệnh `WiFi.*`/`server.*` chỉ chạy trong `webTask`; mọi I/O SD qua `SdLock`; bộ đệm lớn đặt ở PSRAM để dành internal RAM cho Wi-Fi/TLS; Wi-Fi **mặc định TẮT lúc boot**.

## 2. Kiến trúc Wi-Fi hiện tại

- **Bật/tắt:** cờ `wifiEnabledRequest` (UI ghi) → `webTask` gọi `applyWifiState()`. Bật từ Settings > WiFi (mở thẳng màn QR) hoặc giữ Dashboard. Tự tắt sau `wifiAutoOffMin` (10 phút) không có client.
- **AP:** SSID `VietHUD-XXXX` (2 byte cuối MAC) hoặc tên tùy chỉnh; WPA2 nếu mật khẩu ≥ 8 ký tự (mặc định `12345678`), ngược lại AP **mở**. IP 192.168.4.1.
- **Captive portal:** DNSServer wildcard + 302 cho `/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `onNotFound`.
- **mDNS:** `viethud.local`.
- **STA / Wi-Fi Manager:** tối đa 5 mạng đã lưu (NVS `netS%d/netP%d`), `wmTick()` quét → chọn RSSI mạnh nhất → xoay vòng 12 s → quét lại sau 15 s. Có mạng đã lưu → `WIFI_AP_STA`. NTP UTC+7.
- **QR trên máy:** `Settings.cpp wifiQrRebuild()` mã hóa `WIFI:T:WPA;S:<ssid>;P:<pass>;;` (`LV_USE_QRCODE=1`).

## 3. Kiến trúc Web hiện tại

- `WebServer` **đồng bộ** của arduino-esp32 (không AsyncTCP), 1 client/lần, cổng 80, trong `webTask` (8 KB stack, WDT).
- Trang: `/` Live (HTML PROGMEM, poll `/api/status` mỗi 500 ms, có WiFi Manager + nút Update data + banner OTA), `/config` (form HTML dựng bằng `snprintf` vào `configBuf[7680]`), `/update` (upload firmware multipart → `Update.h`), `/triplog`, `/triplog/get` (stream chunk 1 KB).
- API: `/api/status` (JSON tự dựng, `statusBuf[2560]`), `/api/speedlimit`, `/api/speedmap/debug`, `/api/action?do=audiotest|demo|clearlogs|dataupdate|otacheck|reboot`, `/api/wifi/scan|saved|add|del`.
- **Cập nhật dữ liệu hiện tại (Mode A):** `dataupdate` → `dataUpdateSchedule()` đặt cờ NVS + reboot → update-mode (chỉ display+SD, STA, TLS `setInsecure`) → tải `manifest.txt` từ `cfg.dataUpdateUrl` (raw GitHub) → diff SHA theo file → tải `<name>.tmp` + SHA-256 tăng dần → rename đè → lưu manifest → reboot. Auto-check (`dataChk`) chỉ so dòng `version` khi STA có mạng, mỗi 30 phút.
- Định dạng `manifest.txt` hiện tại: dòng `version 2026.09.26.0221` + 7 dòng `<name> <size> <sha256>` (cameras, signs, tiles, index, metadata, names, seg_names).

## 4. Module tái sử dụng được (giữ nguyên hoặc gần nguyên)

| Module | Tái sử dụng cho |
|---|---|
| `applyWifiState()` / `wifiEnabledRequest` pattern | Bật AP cho Bridge; giữ nguyên quy tắc "Wi-Fi chỉ trong webTask" |
| `webPortalApSsid()` + `wifiQrRebuild()` | QR provisioning (chỉ thêm QR thứ 2 cho URL) |
| Wi-Fi Manager (`webPortalAddNetwork/DeleteNetwork`, `/api/wifi/*`) | Mục "Nâng cao / Tùy chọn" — Mode A |
| `DataUpdater` HTTP streaming + mbedtls SHA-256 + update-mode reboot | Mode A (đổi đích ghi sang staging) |
| `SdLock`, `sdMgrReadFileChunk`, `sdMgrRemove` | Staging, readback verify |
| `/triplog/get` chunked-send pattern | Mẫu cho `GET` file lớn |
| `hexEncode`, `parseManifest` (giữ cho tương thích `manifest.txt`) | Mode A fallback, thiết bị cũ |
| Update-mode screen (`runDataUpdateMode`) | Mẫu màn hình "Đang cài đặt dữ liệu" lúc boot |
| Pipeline Pi `tools/viethud-pipeline/github_and_notify.py` | Sinh thêm `manifest.json` + chữ ký + gói `.vhpkg` |

## 5. Module cần sửa

| File | Thay đổi | Lý do |
|---|---|---|
| `net/WebPortal.cpp` | Tách phần xử lý update/firmware ra module mới; thêm AP profile (Captive vs Bridge); **tạm dừng `wmTick()` khi có phiên Bridge hoặc có client AP đang hoạt động**; hoãn auto-off khi có phiên; `esp_task_wdt_reset()` trong raw handler; route `/api/v1/*`; CORS preflight không cần (cùng origin) | C1, C2, C6 |
| `net/DataUpdater.cpp` | Mode A ghi vào **staging session** thay vì rename đè; đọc `manifest.json` (+sig) nếu có, fallback `manifest.txt` | C3, C4 |
| `map/SdCardManager.cpp/.h` | Thêm writer giữ file handle mở (`sdMgrWriterOpen/Write/Close`), `sdMgrFreeBytes()`, `sdMgrMkdir`, `sdMgrExists`; `sdMgrRename` không còn `remove(to)` ngầm (journal lo) | C5, C3 |
| `main_viethud.cpp` | Gọi `dataInstallerRecoverAndApply()` **ngay sau `ensureSdMmcBegun` và trước `sdMgrMount()`**; gọi `firmwareHealthMarkLater()`; update-mode dùng staging | C3, C4, rollback |
| `ui/Settings.cpp` | Màn QR: thêm bước 2 (QR URL `http://192.168.4.1`), dòng trạng thái Bridge ("Điện thoại đang truyền dữ liệu… 62%"); chặn đóng overlay khi đang commit | UX |
| `core/AppConfig.h` + `NvsStore.cpp` | `apProfile` (auto/captive/bridge), cờ đã sinh mật khẩu AP ngẫu nhiên, `dataChannel` (stable) | Bảo mật, cấu hình |
| `include/…` / `platformio.ini` | Thêm `src/update/*` vào `build_src_filter` (đây là **allowlist** — thư mục mới không tự được biên dịch) | Build |
| `tools/viethud-pipeline/github_and_notify.py` | Sinh `manifest.json`, ký, đóng gói `.vhpkg`, publish vào **raw theo tag** + Release | Manifest v2 |

## 6. Module cần tạo mới

```text
src/update/
  UpdateManifest.{h,cpp}   parse manifest.json (JSON tối giản, không ArduinoJson),
                           verify chữ ký ECDSA P-256 (mbedtls_pk, CONFIG_MBEDTLS_ECDSA_C=1 [ĐÃ KIỂM])
  UpdateSession.{h,cpp}    phiên staging: /vhupd/session.json, *.part, offset, trạng thái
  DatasetValidator.{h,cpp} kiểm định dạng: magic/version/header vs size (tiles/index/names/
                           seg_names/signs/cameras/metadata), index sắp xếp, cross-check count
  DataInstaller.{h,cpp}    commit journal + apply lúc boot + rollback + installed.json
  FirmwareUpdater.{h,cpp}  OTA firmware tách riêng: sha256 + chữ ký + verifyRollbackLater()
src/net/
  UpdateApi.{h,cpp}        handler /api/v1/update/*, /api/v1/firmware/*, /api/v1/device
  PortalAssets.h           HTML/JS portal mới (PROGMEM, gzip) — xem §11
  ApProfile.{h,cpp}        cấu hình DHCP (OFFER_ROUTER/OFFER_DNS) + DNS chọn lọc
tools/viethud-pipeline/
  sign_manifest.py         ký manifest (khóa riêng chỉ trên Pi, không commit)
  make_vhpkg.py            đóng gói .vhpkg cho B3
test/
  test_update_manifest/    host test parse + verify chữ ký (vector có sẵn)
  test_data_installer/     host test journal: mô phỏng mất điện ở mọi bước
```

## 7. Kiến trúc Phone Update Bridge

```text
                        GitHub  (nguồn chính thức)
          ┌──────────────────────────────────────────────┐
          │ Release vX  : viethud-data-X.vhpkg (B3, người dùng tải tay) │
          │ raw/<tag>/speedmap/ : manifest.json, .sig, *.bin (CORS *)   │
          └───────┬─────────────────────────────┬────────┘
                  │ HTTPS (Mode A, TLS trên ESP32)│ HTTPS (4G của điện thoại)
                  ▼                              ▼
          ┌──────────────┐   Wi-Fi local   ┌──────────────────────┐
          │   VietHUD    │◄───────────────►│ Phone: trình duyệt    │
          │  ESP32-S3    │  http://192.168.4.1  trang portal do    │
          │              │  /api/v1/update │ ESP32 phục vụ (JS)   │
          └──────────────┘                 └──────────────────────┘
```

**Nguyên tắc:** trang portal (HTML/JS) luôn do **ESP32 phục vụ** từ origin `http://192.168.4.1`. Chính JS đó `fetch()` sang GitHub (cross-origin, cần CORS) và `fetch()` về thiết bị (same-origin, không cần CORS). Không có cloud trung gian.

### 7.1 Ba tầng vận chuyển và bộ chọn tự động

```text
Mở tab "Dữ liệu" → GET /api/v1/update/state (thiết bị) → biết bản đang cài
        │
        ├─ Thiết bị có STA Internet? ── có → đề xuất Mode A (thiết bị tự tải)
        │
        ├─ Probe: fetch(raw/<channel>/manifest.json, timeout 6s)
        │     thành công → B1 (song song): tải → kiểm → đẩy xuống ngay
        │     thất bại   → hiển thị 2 lựa chọn:
        │         B2 "Tải bằng 4G rồi quay lại" (hướng dẫn từng bước)
        │         B3 "Tôi đã có file cập nhật" (<input type=file>)
        │
        └─ Không có gì → "Chế độ offline — cấu hình vẫn dùng bình thường"
```

**B2 chi tiết:** trang lưu `{manifest, sig, danh sách file cần}` vào IndexedDB (IndexedDB **có** trên origin http — không cần secure context) → hướng dẫn "Tạm ngắt Wi-Fi VietHUD" → trang poll `fetch(GitHub)` mỗi 2 s; khi thành công thì tải và ghi từng file (Blob) vào IndexedDB → "Nối lại Wi-Fi VietHUD" → trang poll `/api/v1/ping`; khi thiết bị trả lời thì upload. Nếu tab bị trình duyệt nạp lại, khi mở lại portal trang thấy dữ liệu dở dang trong IndexedDB và tiếp tục. Giới hạn: trình duyệt phải giữ tab (khi chuyển sang app Cài đặt, iOS có thể xóa tab khỏi bộ nhớ — IndexedDB vẫn còn nên chỉ mất tiến độ đang tải của file hiện tại).

**B3 chi tiết:** `.vhpkg` = 1 file duy nhất chứa manifest ký + các `.bin`. JS dùng `File.slice()` để tách và upload theo **đúng protocol B1/B2** — thiết bị không cần biết file đến từ đâu.

### 7.2 AP profile — đòn bẩy quyết định B1

| Profile | DHCP gateway | DHCP DNS | DNS server ESP32 | Hệ quả mong đợi |
|---|---|---|---|---|
| **Captive** (hiện tại) | có | có (192.168.4.1) | wildcard `*` → 192.168.4.1 | Tự bật trang đăng nhập; **chặn** Internet của điện thoại qua Wi-Fi; B1 gần như chắc chắn hỏng nếu OS gửi DNS qua Wi-Fi |
| **Bridge** (mới) | **không** | **không** | chỉ trả `viethud.local`/`viethud.lan`, còn lại REFUSED | OS coi Wi-Fi là mạng "local-only", **4G vẫn là mặc định** cho Internet; 192.168.4.0/24 vẫn là on-link **[CẦN ĐO]** |

Không có gateway thì cửa sổ captive **không tự mở** → bù bằng **QR thứ hai** (URL) trên màn hình máy và dòng chữ `192.168.4.1`. Việc chọn profile mặc định (hoặc "auto": Captive khi chỉ cấu hình, Bridge khi vào mục Dữ liệu — đổi profile buộc điện thoại gia hạn DHCP, nên cần đo xem có mượt không) được **chốt sau Phase 0**.

## 8. Protocol Phone ↔ ESP32

- Transport: HTTP/1.1 trên `192.168.4.1:80`, same-origin, JSON cho điều khiển, **body nhị phân thô** (`application/octet-stream`) cho dữ liệu — xử lý bằng `server.on(path, HTTP_PUT, done, rawHandler)` / `HTTPRaw` (có sẵn trong WebServer core 2.0.17, buffer `HTTP_RAW_BUFLEN=1436` — [ĐÃ KIỂM]). **Không multipart, không nạp cả file vào RAM**: mỗi mảnh 1436 B được ghi thẳng xuống SD.
- **Resumable ngay từ Phase 1** (chi phí thấp vì offset = kích thước `.part` trên SD):
  1. `POST session` với manifest + chữ ký → thiết bị xác minh chữ ký, so với `installed.json`, trả danh sách file cần và **offset đã nhận** của từng file (0 nếu mới; >0 nếu phiên cũ còn dở và cùng `manifestSha`).
  2. `PUT file?name=X&offset=N` thân ≤ 256 KB. Thiết bị chấp nhận chỉ khi `N == size(.part)`, nếu không → `409` + `{"expected":M}` → client nhảy về M. Rớt Wi-Fi ở 20–30 MB → nối lại → `GET session` → tiếp từ offset thật.
  3. Mỗi PUT được trả `{"received":N+len}`; client dùng làm tiến độ.
  4. `POST commit` → thiết bị readback toàn file để tính SHA-256 (verify đúng thứ đã nằm trên thẻ, không tin hash tăng dần), kiểm định dạng, ghi journal, trả `202`, sau đó reboot để áp dụng.
- Không lưu mbedtls context giữa các lần resume: tính SHA lúc commit bằng readback (≈ 10 MB đọc SD ≈ 1–3 s) — đơn giản và bắt được lỗi ghi thẻ.
- Chống tranh chấp: **chỉ 1 phiên** tại một thời điểm, `sessionId` 128-bit ngẫu nhiên (`esp_random`), hết hạn sau 30 phút không hoạt động (lưu trên SD nên sống qua reboot).

## 9. API specification (v1)

Tiền tố `/api/v1`. API cũ (`/api/status`, `/api/action`, `/api/wifi/*`, `/update`) **giữ nguyên** để không phá trang/thói quen hiện tại; trang mới dùng v1.

| Method | Path | Mô tả | Trả về |
|---|---|---|---|
| GET | `/ping` | Kiểm tra kết nối nhanh (B2 dùng) | `{"ok":true,"id":"43F0"}` |
| GET | `/device` | Model, firmware version, board id, free SD, AP profile, STA state | JSON |
| GET | `/update/state` | Bản đang cài theo dataset + trạng thái phiên + trạng thái Mode A | `{"datasets":{"map":{"version":"2026.10.01"},"alerts":{"version":…}},"session":null|{…},"direct":{"internet":false}}` |
| POST | `/update/session` | Body JSON `{"manifest":"<nguyên văn>","sig":"<base64>","datasets":["alerts"]}` | `201 {"sid":"…","files":[{"name":"signs.bin","size":…,"received":0}],"chunkMax":262144}` / `400 bad_signature` / `409 busy` / `422 min_firmware` / `507 no_space` |
| GET | `/update/session` | Tiến độ phiên hiện tại (để resume) | như trên + `state` |
| PUT | `/update/file?sid=&name=&offset=` | Thân nhị phân ≤ `chunkMax` | `200 {"received":N}` / `409 {"expected":M}` / `413` / `410 session_expired` |
| POST | `/update/commit?sid=` | Verify + validate + ghi journal | `202 {"state":"verifying"}`; poll `/update/session` → `installing` → thiết bị reboot |
| DELETE | `/update/session?sid=` | Hủy, xóa staging | `204` |
| POST | `/update/direct` | Mode A: yêu cầu thiết bị tự tải (lên lịch update-mode) | `202` / `424 no_internet` |
| GET | `/firmware/state` | Bản firmware đang chạy/slot/rollback pending | JSON |
| POST/PUT/POST | `/firmware/session`, `/firmware/chunk`, `/firmware/commit` | Tách riêng khỏi dữ liệu (§14.4) | tương tự |

Mã lỗi thống nhất: `{"error":"<code>","detail":"…"}`. Trong khi có phiên `uploading`, `GET /api/status` vẫn trả lời nhưng trang mới ngừng poll để không tranh 1 kết nối duy nhất của WebServer.

### 9.1 Manifest v2 (`manifest.json`) + chữ ký

```json
{
  "schema": 2,
  "channel": "stable",
  "published": "2026-10-01T03:00:00Z",
  "minFirmware": "2.1.0",
  "baseUrl": "https://raw.githubusercontent.com/911273/VietHUD/data-2026.10.01/speedmap/",
  "datasets": {
    "map": {
      "version": "2026.10.01",
      "files": [
        {"name": "tiles.bin",     "size": 5102776, "sha256": "…"},
        {"name": "index.bin",     "size": 96236,   "sha256": "…"},
        {"name": "metadata.bin",  "size": 140,     "sha256": "…"},
        {"name": "names.bin",     "size": 144563,  "sha256": "…"},
        {"name": "seg_names.bin", "size": 728972,  "sha256": "…"}
      ]
    },
    "alerts": {
      "version": "2026.09.26",
      "files": [
        {"name": "signs.bin",   "size": 2687920, "sha256": "…"},
        {"name": "cameras.bin", "size": 2031240, "sha256": "…"}
      ]
    }
  }
}
```

- **Dataset là đơn vị cài đặt nguyên tử.** `map` gom 5 file phụ thuộc nhau (names/seg_names/index phải khớp tiles). `alerts` độc lập (điểm, không tham chiếu segment) nên cập nhật riêng được — đúng yêu cầu "chỉ tải cái cần".
- `manifest.json.sig` = ECDSA P-256/SHA-256 trên **byte nguyên văn** của `manifest.json`. Khóa riêng chỉ ở Pi; khóa công khai hằng trong firmware (có thể nhúng 2 khóa để xoay vòng).
- `baseUrl` trỏ vào **tag bất biến** (không phải `main`) → file không bị thay giữa lúc tải; kênh "latest" là `raw/main/speedmap/channel-stable.json` chỉ chứa `{"manifest":"<url theo tag>"}`.
- Pipeline vẫn sinh **`manifest.txt` cũ** song song để thiết bị firmware cũ (V1, bản hiện tại) tiếp tục cập nhật được.
- Release `data-2026.10.01` đính kèm `viethud-data-2026.10.01.vhpkg` cho B3 (Release là bản ghi chính thức; raw theo tag là "mirror có CORS" của cùng byte, được bảo đảm bởi cùng chữ ký).

## 10. Data update state machine

**Thiết bị (một nguồn sự thật, lưu `/vhupd/session.json`):**

```text
IDLE ──POST session (sig ok, space ok)──► RECEIVING ──PUT…(offset khớp)──► RECEIVING
  ▲                                         │ mọi file đủ size
  │ DELETE / hết hạn 30'                    ▼
  ├────────────────────────────────── READY ──POST commit──► VERIFYING
  │                                                      │ sha ok + định dạng ok
  │ lỗi (sha/format) → FAILED(lý do) → xóa .part ◄───────┤
  │                                                      ▼
  │                                   COMMIT_PENDING (ghi journal) ──reboot──►
  │   [BOOT, trước sdMgrMount] APPLYING: .cur→.bak, .new→.cur theo journal (redo được)
  │                           → sdMgrMount + DatasetValidator
  │           ok → INSTALLED (xóa .bak, cập nhật installed.json, xóa journal)
  └───────────── lỗi mount → ROLLED_BACK (.bak→.cur) → báo lỗi trên màn hình + portal
```

**Điện thoại:**

```text
DETECT → (A khả dụng? → DIRECT) | (B1 probe ok → FETCH_MANIFEST) | (B2/B3 lựa chọn) | OFFLINE
FETCH_MANIFEST → COMPARE → UP_TO_DATE | NEED[datasets]
NEED → DOWNLOADING(sha JS mỗi file; sai → dừng, KHÔNG upload) → OPEN_SESSION
     → TRANSFERRING(resume theo offset) → COMMITTING → WAIT_REBOOT(poll /ping) → DONE
```

## 11. UI/UX sitemap

**Trên máy (Settings > Wi-Fi, màn QR hiện có):**

```text
[Bước 1] QR Wi-Fi  (WIFI:T:WPA;S:VietHUD-43F0;P:<ngẫu nhiên>;;)
[Bước 2] QR URL    (http://192.168.4.1)      + chữ: 192.168.4.1
Trạng thái: "Điện thoại đã kết nối" / "Đang nhận dữ liệu 62%" / "Đang kiểm tra…" /
            "Sẽ khởi động lại để cài đặt"
```

**Portal (một trang SPA nhỏ, gzip trong PROGMEM, không tài nguyên ngoài — chạy khi không có Internet):**

```text
Trang chủ ─┬─ Dữ liệu (mặc định mở tab này)     ◄── ưu tiên số 1
           │     Internet: ● Điện thoại (4G/5G)  | ● VietHUD Wi-Fi | ○ Không có
           │     Bản đồ    2026.10.01 ✓
           │     Cảnh báo  2026.09.25 → 2026.09.26
           │     [ Kiểm tra cập nhật ]  → [ Cập nhật (2.6 MB) ]
           │     Tiến độ: Tải về điện thoại 84% → Truyền sang VietHUD 62%
           │              → Kiểm tra ✓ → Cài đặt ✓ → Hoàn tất
           │     Link phụ: "Không có mạng khi đang nối VietHUD?" (B2) · "Tôi có file" (B3)
           ├─ Trạng thái (Live hiện tại: GNSS, speed map, nhiệt độ, RAM)
           ├─ Cài đặt (form /config hiện tại)
           ├─ Nâng cao / Tùy chọn
           │     Wi-Fi Internet cho VietHUD (Wi-Fi Manager hiện tại) — Mode A
           │     Tên/mật khẩu hotspot VietHUD · AP profile
           │     Firmware (tách riêng, có mã PIN)
           └─ Thông tin hệ thống (version, board id, SD, trip logs)
```

Người dùng không thấy chữ GitHub/OSM/manifest/checksum; chỉ thấy "Bản đồ", "Cảnh báo", "Kiểm tra cập nhật". Nhãn trạng thái Internet được **xác định ở client** (probe GitHub từ trình duyệt thành công ⇒ "Điện thoại có Internet"; thiết bị báo STA+NTP ⇒ "VietHUD Wi-Fi"). ESP32 không cố đoán điện thoại dùng 4G.

Tuân thủ quy tắc sản phẩm sẵn có: không hiển thị điều thiết bị không thể biết (không ghi "Đã cập nhật mới nhất" khi chưa hỏi được nguồn — ghi "Chưa kiểm tra được").

## 12. Phân tích tương thích Android / iOS / Browser

### 12.A Android

| Hành vi | Kỳ vọng | Độ chắc |
|---|---|---|
| Nối AP không Internet | Android kiểm tra (probe `generate_204`) trên Wi-Fi; thất bại → "Wi-Fi không có Internet", **4G giữ làm mạng mặc định** | cao, [CẦN ĐO] trên máy người dùng (Honor Magic V3) |
| Hộp thoại "Giữ kết nối Wi-Fi?" | Nếu người dùng chọn "Có/Không hỏi lại", Android có thể **ưu tiên Wi-Fi làm mặc định** → mất 4G cho trình duyệt | [CẦN ĐO] — portal phải hướng dẫn chọn đúng |
| Trình duyệt truy cập 192.168.4.1 khi mặc định là 4G | Socket không bind của Chrome đi theo mạng mặc định; truy cập subnet on-link của Wi-Fi không-mặc-định **tùy phiên bản Android/OEM** | **thấp — rủi ro chính của B1**, [CẦN ĐO] |
| Captive profile | Hiện "Đăng nhập mạng", mở **CaptivePortalLogin** (WebView bị ràng buộc vào Wi-Fi) → trong WebView này **không tới được GitHub** | cao |
| Cài đặt dev "Mobile data always active" | Mặc định BẬT trên đa số máy (giúp chuyển mạng nhanh) | trung bình |

Kết luận Android: B1 **có thể** chạy trong Chrome với profile Bridge, nhưng không được giả định. Native app giải quyết triệt để bằng `ConnectivityManager.requestNetwork(TRANSPORT_WIFI)` + `Network.openConnection()` cho socket local và mạng mặc định cho GitHub (§12.D).

### 12.B iOS

> **Cập nhật lần 2:** phân tích sâu cho iPhone (định tuyến kiểu BSD, điều kiện để 4G + Wi-Fi VietHUD chạy song song, bẫy của firmware hiện tại, và bài test làm ngay được) nằm ở **§19**. Tóm tắt: với iPhone, B1 **rất có khả năng chạy được**, *với điều kiện* AP không quảng bá router và không bật captive portal. Firmware hiện tại lại làm đúng hai việc đó.

| Hành vi | Kỳ vọng | Độ chắc |
|---|---|---|
| Captive profile | Mở **CNA sheet** (trình duyệt mini): không tải file, không IndexedDB bền, đóng là mất; traffic đi Wi-Fi → **không dùng cho update** | cao |
| Wi-Fi không có router (profile Bridge) | Theo hướng dẫn dành cho phụ kiện Wi-Fi, iOS **tiếp tục dùng cellular cho Internet** khi mạng Wi-Fi không cấp router, vẫn đi on-link tới 192.168.4.x | trung bình, [CẦN ĐO] — đây là giả thuyết cốt lõi của B1 trên iPhone |
| Wi-Fi có router nhưng không Internet | iOS gắn nhãn "Không có kết nối Internet"; việc Safari có tự đi qua 4G hay không **không nhất quán** giữa các bản iOS | thấp |
| "Wi-Fi Assist" | Chỉ can thiệp khi Wi-Fi yếu, không phải cơ chế đáng tin cho case này | — |
| Local Network permission (iOS 14+) | Áp dụng cho **app** truy cập LAN; Safari tự có quyền | cao |
| Safari giữ tab khi chuyển sang Cài đặt | Thường giữ; có thể nạp lại khi thiếu RAM → B2 dựa vào IndexedDB | trung bình |
| Hotspot iPhone cho Mode A | Chỉ phát 2.4 GHz khi bật "Tối đa hóa tương thích"; radio ngủ khi không ở màn Hotspot (đã gặp thực tế — xem `runDataUpdateMode`) | cao (đã gặp) |

### 12.C Browser (chung)

| Khả năng | Trạng thái | Ghi chú |
|---|---|---|
| Mở `http://192.168.4.1` | ✓ | Chrome/Safari có thể cảnh báo "Không bảo mật" — chấp nhận được |
| `fetch()` GitHub từ trang http | ✓ | https từ trang http **không** bị chặn mixed-content (chiều ngược lại mới bị) |
| CORS `raw.githubusercontent.com` | ✓ **[ĐÃ KIỂM]** `Access-Control-Allow-Origin: *` | Nguồn tải cho B1/B2 |
| CORS GitHub Release asset | ✗ **[ĐÃ KIỂM]** redirect tới `release-assets.githubusercontent.com` không có header CORS | Chỉ dùng cho B3 (tải bằng điều hướng, không `fetch`) |
| CORS `api.github.com` | ✓ (ACAO `*`) | Không cần — giới hạn 60 req/h/IP khi ẩn danh, tránh dùng |
| jsDelivr `cdn.jsdelivr.net/gh/…@tag` | ✓ CORS, giới hạn ~20 MB/file | Mirror dự phòng tùy chọn |
| Private Network Access (Chrome) | Không ảnh hưởng | Trang private → public là hợp lệ; chỉ public → private mới bị chặn |
| `crypto.subtle` | ✗ trên http | SHA-256 bằng JS thuần (~3 KB, stream theo chunk) |
| Service Worker / Cache API / OPFS / cài PWA | ✗ trên http | → §13 |
| IndexedDB, Blob, `File.slice`, `<input type=file>` | ✓ | Dùng cho B2, B3 |
| `fetch` upload body `Blob`/`ArrayBuffer` | ✓ | Không có tiến độ upload trong `fetch`; chia chunk 256 KB → tiến độ theo chunk (hoặc XHR `upload.onprogress`) |
| `navigator.onLine` | Không dùng được | Chỉ phản ánh có interface, không phải có Internet → phải probe thật |
| Kết nối song song tới ESP32 | Tránh | WebServer đồng bộ 1 client; `CONFIG_LWIP_MAX_SOCKETS=16` [ĐÃ KIỂM] — client chạy tuần tự |

### 12.D Native app fallback (Phase 3, chỉ khi Phase 0/1 cho thấy cần)

- Dùng **y nguyên API v1** (§9) — app chỉ là client khác; không cần đổi firmware.
- Android: `requestNetwork(TRANSPORT_WIFI, không NET_CAPABILITY_INTERNET)` → gửi request tới 192.168.4.1 qua `network.socketFactory`; GitHub qua mạng mặc định. Hoặc `WifiNetworkSpecifier` để nối AP mà không đổi mạng mặc định của hệ thống.
- iOS: `NEHotspotConfiguration` để nối AP; request local qua `NWConnection` với `requiredInterfaceType = .wifi`, GitHub với `.cellular`; khai báo `NSLocalNetworkUsageDescription`.
- Khám phá thiết bị: mDNS `_viethud._tcp` (thêm `MDNS.addService("viethud","tcp",80)` với TXT `id`, `fw`).
- Có thể thêm WebSocket/ SSE tiến độ về sau; không bắt buộc.

## 13. Tính khả thi PWA

- **PWA do ESP32 phục vụ: không khả thi.** Origin `http://192.168.4.1` không phải secure context → không Service Worker → không cài đặt, không offline shell. ESP32 phục vụ HTTPS với chứng chỉ tự ký thì trình duyệt không chấp nhận SW trên chứng chỉ không tin cậy.
- **PWA đặt trên GitHub Pages (https): khả thi một phần.** Nó cài được, cache được, tải GitHub tốt, lưu gói vào OPFS/Cache — nhưng **không thể gọi `http://192.168.4.1`** (mixed content bị chặn; Chrome còn chặn public→private bởi Private Network Access). Nên chỉ đóng vai "download manager": tải sẵn gói `.vhpkg` rồi người dùng **chia sẻ/lưu file** và dùng B3 trên portal. Giá trị thêm so với B3 thuần (tải từ trang Release) là nhỏ.
- **Khuyến nghị:** Phase 2 **không làm PWA**; thay vào đó hoàn thiện B2/B3 trong portal. Chỉ cân nhắc PWA-trên-Pages nếu người dùng muốn "tải sẵn ở nhà, cập nhật ngoài xe" nhiều lần. Kiểm soát mạng thật sự cần native app (§12.D).

## 14. Security model

### 14.1 Tài sản & mối đe dọa

| Tài sản | Mối đe dọa | Biện pháp |
|---|---|---|
| Dữ liệu map/cảnh báo | Người lạ trong tầm AP upload dữ liệu giả (vd. xóa camera, giới hạn tốc độ sai → nguy hiểm khi lái) | **Chữ ký ECDSA manifest verify trên ESP32** + SHA-256 từng file; không có chữ ký hợp lệ ⇒ `400`, không staging |
| Firmware | Flash firmware lạ qua `/update` (hiện **không có bảo vệ**) | Chữ ký firmware (cùng khóa hoặc khóa riêng) + mã PIN thiết bị hiển thị trên màn hình cho mục Firmware; giữ `/update` cũ sau cờ build dev |
| Truy cập AP | Mật khẩu mặc định `12345678` dùng chung mọi máy | Sinh **mật khẩu ngẫu nhiên 10 ký tự lúc boot đầu** (lưu NVS), hiển thị qua QR; người dùng đổi được trong Nâng cao |
| Mật khẩu Wi-Fi nhà (Mode A) | Lộ qua portal | Giữ nguyên: không echo mật khẩu ra HTML (đã làm); API không bao giờ trả mật khẩu |
| Rollback tấn công (cài bản cũ có lỗi) | Upload manifest ký hợp lệ nhưng cũ | Từ chối `version` < bản đang cài trừ khi bật "cho phép hạ cấp" trên **màn hình máy** |
| CSRF từ trang khác | Trang web lạ gọi `192.168.4.1` khi điện thoại đang nối AP | Endpoint ghi yêu cầu header `X-VietHUD: 1` + `Content-Type: application/json` (bắt buộc preflight cross-origin mà ESP32 không trả lời ⇒ bị chặn) |
| Riêng tư | Cloud trung gian | Không có: chỉ Phone↔GitHub và Phone↔ESP32 |

### 14.2 Phía điện thoại
Kiểm `size` + SHA-256 (JS) trước khi upload; sai ⇒ **không upload**, báo "Tải về bị lỗi, thử lại". Điện thoại **không** là gốc tin cậy.

### 14.3 TLS trên ESP32 (Mode A)
Hiện dùng `setInsecure()`. Vì manifest đã ký, MITM chỉ có thể làm **từ chối dịch vụ**, không thể cài dữ liệu giả ⇒ chấp nhận được; tùy chọn nhúng root CA sau.

### 14.4 Firmware update tách biệt
Luồng riêng `/api/v1/firmware/*`, manifest riêng (`firmware.json`: version, size, sha256, sig, minBootloader). Dùng rollback của bootloader: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1` **[ĐÃ KIỂM]** và core có hook yếu `verifyRollbackLater()` **[ĐÃ KIỂM]** (`esp32-hal-misc.c:207`) — override trả `true`, rồi gọi `esp_ota_mark_app_valid_cancel_rollback()` sau khi chạy ổn định 60 s (UI + GNSS + SD ok); reset trước đó ⇒ bootloader tự quay về slot cũ. Dữ liệu và firmware không bao giờ nằm chung một gói.

## 15. Tác động bộ nhớ / CPU

| Hạng mục | Ước tính | Nơi | Ghi chú |
|---|---|---|---|
| Portal SPA (gzip) | 25–40 KB | Flash (PROGMEM) | App slot 6.25 MB, firmware hiện ~29% |
| Raw upload buffer | 1.4 KB | internal (có sẵn trong WebServer) | Không thêm |
| Session + manifest parse | ~6 KB | **PSRAM** | Manifest ≤ 4 KB, bảng file ≤ 24 mục |
| Writer file mở | ~0.5 KB | internal (FILE của VFS) | |
| ECDSA verify | ~3–5 KB stack tạm, ~150–300 ms CPU | webTask | Chỉ 1 lần/phiên → tăng stack webTask 8 K→12 K, đo high-water |
| SHA-256 readback commit | ~10 MB đọc SD ≈ 1–3 s | webTask, giữ SdLock theo chunk 4 KB | Nhả SdLock giữa chunk để speedLimitTask vẫn đọc tile |
| Upload song song với lái xe | Ghi SD cạnh tranh với đọc tile | SdLock | Cho phép nhận khi đang chạy nhưng **commit/reboot chỉ khi tốc độ < 3 km/h** (hoặc hỏi xác nhận trên máy) |
| Mode B không cần TLS trên ESP32 | **0 KB** TLS | — | Lợi thế lớn: không cần update-mode reboot để tải |
| Thông lượng dự kiến | 300–800 KB/s (SoftAP + SD 1-bit) | | 10 MB ≈ 15–40 s; bottleneck là ghi SD — **cần đo Phase 0** |
| Dung lượng SD staging | ≤ 2× dataset (`.part` + `.bak`) ≈ 25 MB | SD | Kiểm `sdMgrFreeBytes()` khi mở phiên, `507` nếu thiếu |

Không thêm task mới cho Bridge: tất cả chạy trong `webTask` hiện có (đúng quy tắc Wi-Fi-một-task). Core 1 (UI) không bị ảnh hưởng.

## 16. Failure recovery

| Sự cố | Trạng thái để lại | Phục hồi |
|---|---|---|
| Rớt Wi-Fi giữa upload | `.part` có N byte, phiên còn hạn | Client `GET session` → tiếp từ N |
| Tab bị đóng / điện thoại khóa | như trên | Mở lại portal → phát hiện phiên dở (theo `manifestSha`) → "Tiếp tục cập nhật?" |
| Mất điện khi RECEIVING | `.part` bất kỳ | Boot bình thường (dữ liệu live không bị động tới); phiên còn trên SD để resume hoặc hết hạn → dọn |
| Mất điện khi VERIFYING | chưa có journal | Như trên; commit lại |
| Mất điện giữa APPLYING | journal ghi rõ từng bước (`cur→bak`, `new→cur`) | Boot đọc journal → **redo** các bước còn thiếu (thao tác idempotent) → validate |
| Dữ liệu mới không mount được | `.bak` còn | Rollback tự động `.bak→.cur`, ghi lý do vào `installed.json.lastError`, hiện thông báo |
| SHA sai / định dạng sai | `.part` bị xóa | Báo lỗi cụ thể; không đụng dữ liệu live |
| Thẻ đầy | — | `507` ngay khi mở phiên, trước khi tải |
| Reboot loop do dữ liệu | bộ đếm boot-fail trong NVS | ≥ 2 lần boot hỏng sau APPLY ⇒ rollback cưỡng bức |
| Firmware mới lỗi | — | Rollback bootloader (§14.4) |
| Auto-off Wi-Fi giữa B2 | AP tắt khi người dùng đi tải bằng 4G | Hoãn auto-off khi có phiên mở (tối đa 30 phút) |
| wmTick scan làm rớt AP | — | Tạm dừng wmTick khi phiên mở hoặc có client AP (C2) |

## 17. Testing matrix

**Phase 0 (đo thực tế, quyết định thiết kế):**

| Điện thoại | Profile | Chế độ người dùng | Đo: truy cập 192.168.4.1 | Đo: fetch raw GitHub | Đo: tốc độ upload |
|---|---|---|---|---|---|
| Android (Honor Magic V3) | Captive | mặc định | | | |
| Android | Captive | chọn "Giữ Wi-Fi" | | | |
| Android | Bridge (no gw/DNS) | mặc định | | | |
| Android khác (Samsung/Xiaomi nếu có) | Bridge | mặc định | | | |
| iPhone | Captive (CNA) | Hủy → "Dùng không có Internet" | | | |
| iPhone | Bridge | mặc định | | | |
| iPhone / Android | bất kỳ | Tắt dữ liệu di động | (phải ✓) | (phải ✗, portal vẫn chạy) | |

Công cụ: một trang probe tạm (`/probe`) trong firmware thử nghiệm + cờ serial `p` đổi profile.

**Phase 1+ (hồi quy & chức năng):**

| Nhóm | Ca |
|---|---|
| Không phá hành vi hiện có | Lái thử 15 phút với Wi-Fi **tắt** (mặc định) — không đổi FPS/flush, không crash; `speedLimitTask` stack high-water; Live/Config/Triplog/WiFi Manager cũ vẫn chạy |
| Upload | 1 file nhỏ, cả map 7 MB, chỉ alerts; chunk sai offset (409); upload song song 2 client (409 busy) |
| Resume | Tắt Wi-Fi điện thoại ở 30%/70%; khóa màn hình; reload tab; reboot ESP32 giữa phiên |
| Mất điện | Rút nguồn ở từng trạng thái RECEIVING/VERIFYING/mỗi bước APPLYING (host test mô phỏng + 10 lần thật) |
| Bảo mật | Manifest sửa 1 byte; chữ ký của khóa khác; file đúng size sai SHA; bản cũ hơn; POST không header `X-VietHUD` |
| Validator | Header sai magic, names v1/v2, count lệch size, index không sắp xếp |
| Mode A | STA qua hotspot iPhone 2.4 GHz; qua Wi-Fi nhà; manifest.txt-only (tương thích ngược) |
| Offline | Điện thoại không Internet: cấu hình, trạng thái, Wi-Fi Manager, trip log đều chạy; tab Dữ liệu báo "Chưa kiểm tra được" chứ không lỗi |
| Tài nguyên | min free internal khi upload + UI + map chạy; WDT không kích hoạt với chunk 256 KB trên Wi-Fi yếu (−80 dBm) |
| Firmware | Firmware lỗi cố ý (panic ở setup) → tự rollback |
| Host tests | `pio test -e native`: parse manifest, verify sig, journal redo, validator |

## 18. Implementation roadmap

Mỗi bước build + flash + đo được độc lập; không bước nào đổi hành vi khi Wi-Fi tắt.

| Phase | Nội dung | Kết quả kiểm chứng | Ước lượng |
|---|---|---|---|
| **P-1 Chuẩn bị** | Commit 8 file đang sửa trên `main` (hoặc tách branch `feature/update-bridge`); ghi chú baseline RAM/stack | Git sạch, có điểm quay lại | 0.5 ngày |
| **P0 Spike tương thích** | Firmware thử: AP profile Bridge (tắt `OFFER_ROUTER`/`OFFER_DNS`, DNS chọn lọc), trang `/probe`, endpoint PUT raw ghi SD bằng writer giữ handle; đo bảng §17 trên iPhone + Android | **Chốt**: B1 dùng được trên máy nào, profile mặc định, chunk size, thông lượng | 1–2 ngày |
| **P1 Nền tảng an toàn** | (1) AP mật khẩu ngẫu nhiên + QR đôi; (2) sửa C2 (pause wmTick), C5 (writer), C6 (WDT); (3) `DataInstaller` journal + apply-at-boot + rollback; (4) chuyển Mode A hiện tại sang staging (sửa C3/C4) | Mode A cũ vẫn chạy nhưng nguyên tử; test mất điện đạt | 3–4 ngày |
| **P2 Manifest v2 + chữ ký** | Pipeline Pi sinh `manifest.json` + `.sig` + `.vhpkg`, publish raw theo tag + Release, giữ `manifest.txt`; firmware `UpdateManifest` + `DatasetValidator` + host test | Thiết bị từ chối manifest giả | 2 ngày |
| **P3 Phone Bridge B1 + B3** | `UpdateApi` v1 + portal SPA tab Dữ liệu (probe, compare, tải, SHA JS, upload resumable, tiến độ), B3 chọn file | Tiêu chí §24: Scan QR → … → tiếp tục dùng, không cấu hình Wi-Fi nhà | 4–5 ngày |
| **P4 B2 tuần tự** | IndexedDB, wizard "ngắt/nối lại", tự tiếp tục sau reload | Chạy trên máy mà Phase 0 cho thấy B1 hỏng | 2 ngày |
| **P5 Direct Update tích hợp** | Nút "Cập nhật" tự chọn A khi thiết bị có STA; Wi-Fi Manager dời vào Nâng cao; màn máy hiển thị tiến độ | Sơ đồ quyết định §21 hoạt động tự động | 1–2 ngày |
| **P6 Firmware update an toàn** | `FirmwareUpdater` (ký + sha + `verifyRollbackLater`), PIN; `/update` cũ chỉ bật ở build dev | Firmware lỗi tự rollback | 2 ngày |
| **P7 (tùy)** | PWA-trên-Pages làm download manager — chỉ khi cần | | — |
| **P8 (tùy)** | Native app dùng API v1 — chỉ khi Phase 0 cho thấy browser không đủ trên thiết bị mục tiêu | | — |

---

## Phụ lục A — Danh sách điểm conflict/crash phải xử lý (checklist code review)

1. C1 DNS wildcard/gateway → AP profile Bridge.
2. C2 `wmTick()` scan phá AP → pause khi phiên mở / có client.
3. C3 rename từng file → journal dataset-level.
4. C4 thay file đang dùng khi chạy → chỉ áp dụng lúc boot trước `sdMgrMount()`.
5. C5 open-append-close → writer giữ handle; `flush()` mỗi 64 KB; đóng khi hết chunk.
6. C6 WDT trong handler dài → `esp_task_wdt_reset()` trong raw callback; giới hạn chunk 256 KB.
7. C7 CORS Release → tải từ raw theo tag; Release chỉ cho B3.
8. C8 không có `crypto.subtle` → SHA-256 JS thuần.
9. `build_src_filter` allowlist → thêm `+<update/*>`, nếu không sẽ lỗi link "undefined reference".
10. `webTask` stack 8 KB + ECDSA → tăng 12 KB, log high-water như `speedLimitTaskStackFreeBytes()`.
11. `statusBuf[2560]` gần đầy nếu thêm trường → dùng endpoint v1 riêng, không nhồi thêm vào `/api/status`.
12. WebServer 1 client: trang mới ngừng poll 500 ms khi đang upload.
13. STA kết nối làm AP đổi kênh → không bật STA trong phiên Bridge.
14. Auto-off 10 phút → hoãn khi có phiên.
15. SD 1-bit chia sẻ với map matcher → SdLock theo chunk, không giữ lock suốt file.

## Phụ lục B — Câu hỏi mở cần người dùng quyết định

1. Đặt tên dataset: tài liệu dùng `map` / `alerts` (trung tính theo nguồn). Lưu ý: nguồn dữ liệu cảnh báo công bố công khai trên GitHub nên có giấy phép phân phối lại rõ ràng (OSM/ODbL + khảo sát riêng như pipeline `tools/viethud-pipeline`); cơ chế cập nhật trong tài liệu này không phụ thuộc nguồn.
2. Cho phép nhận dữ liệu khi xe đang chạy (commit chỉ khi dừng) hay chặn hoàn toàn?
3. Mã PIN cho firmware update: hiển thị trên màn hình mỗi lần, hay cố định?
4. Kênh phát hành: chỉ `stable`, hay thêm `beta`?

---

## 19. Bổ sung lần 2 (2026-09-26): iPhone — Wi-Fi VietHUD + 4G cùng lúc

Rà lại code trên máy: **không có thay đổi** kể từ bản kế hoạch (vẫn 8 file chưa commit trên `main`). Đọc thêm phần DHCP của SoftAP trong core 2.0.17 (`WiFiGeneric.cpp set_esp_interface_ip`) và luồng QR (`Settings.cpp wifiQrRebuild/onWifiScanOpen`).

### 19.1 Vì sao nhận định "iPhone dùng được cả hai" là có cơ sở

iOS có nhân gốc BSD và **định tuyến theo địa chỉ đích**, không theo từng app/mạng như Android:

```text
Bảng định tuyến iPhone khi nối AP VietHUD và có 4G:
  192.168.4.0/24  → en0 (Wi-Fi)       route on-link, luôn tồn tại khi đã nối AP
  default         → ? (en0 hoặc pdp_ip0 = cellular)   ← chỉ điểm này thay đổi
```

- `http://192.168.4.1` **luôn** đi ra en0 vì route /24 cụ thể hơn route default, bất kể mạng nào là "primary".
- `raw.githubusercontent.com` đi theo **route default và DNS của mạng primary**.
- Như vậy câu hỏi duy nhất là **iOS có chọn Wi-Fi VietHUD làm primary hay không**. Nếu 4G vẫn là primary thì B1 chạy đúng như bạn nghĩ.

iOS chọn primary dựa trên: (a) DHCP có cấp **router** hay không, (b) kết quả probe `captive.apple.com/hotspot-detect.html`, (c) lựa chọn của người dùng trong màn captive (CNA).

| Cấu hình AP | Kết quả dự kiến trên iPhone | B1 |
|---|---|---|
| **Không có router** trong DHCP | Wi-Fi không thể làm default route → **4G giữ primary**; 192.168.4.1 vẫn đi Wi-Fi | ✓ khả năng cao — cấu hình mà các phụ kiện Wi-Fi (camera hành trình, drone) hay dùng |
| Có router, probe trả **302 → captive** (**firmware hiện tại**) | Mở màn CNA; CNA chỉ đi qua Wi-Fi. Người dùng bấm *Hủy → Dùng không có Internet* thì hành vi sau đó **không nhất quán giữa các bản iOS** | ? cần đo |
| Có router, probe không có phản hồi | Gắn nhãn "Không có kết nối Internet"; có thể vẫn ưu tiên Wi-Fi | ? cần đo |
| Có router, probe thành công | Wi-Fi primary (Wi-Fi nhà bình thường) | ✗ (không áp dụng) |

### 19.2 Bẫy của firmware hiện tại đối với iPhone (đọc từ code)

1. **DHCP có cấp router.** `WiFi.softAP()` gọi `set_esp_interface_ip()`, hàm này chỉ đặt dải lease rồi `dhcps_start`. dhcpserver của IDF 4.4 mặc định bật `OFFER_ROUTER` (router = 192.168.4.1). Có thể tắt bằng `esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer=0)` kẹp giữa `esp_netif_dhcps_stop()` và `esp_netif_dhcps_start()`, ngay sau `WiFi.softAP()` trong `applyWifiState()`. Lưu ý: dhcpserver 4.4 **luôn cấp DNS** (IP của AP) — không tắt được bằng cờ, nhưng không sao vì iOS chỉ dùng DNS của mạng primary cho tên miền thường.
2. **Captive portal chủ động kích hoạt CNA:** route `/hotspot-detect.html` trả 302 và `onNotFound` cũng 302 (`WebPortal.cpp:1053-1055`). CNA là trình duyệt thu nhỏ: chỉ đi qua Wi-Fi, không tải được file, đóng là mất trạng thái. **Cập nhật không bao giờ được chạy trong CNA.**
3. **DNS wildcard** (`dnsServer.start(53,"*",…)`): nếu vì lý do nào đó Wi-Fi thành primary, `raw.githubusercontent.com` → 192.168.4.1:443. ESP32 không nghe cổng 443 nên kết nối bị từ chối ngay → fetch lỗi nhanh (không treo). Portal phải hiểu lỗi này là "Internet đang đi qua VietHUD" và hướng dẫn đúng.
4. **QR hiện tại chỉ có lệnh nối Wi-Fi.** Không có QR mở URL → người dùng phụ thuộc vào CNA (bẫy 2).

### 19.3 Thay đổi kế hoạch vì kết luận này

| Mục | Trước | Sau |
|---|---|---|
| Đường chính cho iPhone | B1 "phụ thuộc OS, cần đo" | **B1 là đường mặc định**; B2/B3 chỉ còn là dự phòng |
| AP profile mặc định | chốt sau Phase 0 | **Bridge (không router, không captive) làm mặc định**. Captive chỉ còn là tùy chọn trong Nâng cao. Lý do: flow của bạn luôn bắt đầu từ QR trên màn máy, nên popup captive gần như không giúp gì, trong khi nó phá chế độ song song |
| QR trên máy | 1 QR Wi-Fi | **2 QR**: ① nối Wi-Fi, ② `http://192.168.4.1`. Camera iPhone mở QR URL bằng **Safari thật**, không phải CNA |
| DNS server ESP32 | wildcard | chỉ trả lời `viethud.local` / `viethud.lan`; tên khác → REFUSED |
| Probe Internet ở portal | fetch + timeout | fetch `manifest.json` + **kiểm nội dung** (parse được, `schema` đúng); phân loại lỗi: *bị từ chối nhanh* (Wi-Fi đang primary) / *timeout* (không có 4G) / *nội dung sai* |
| Roadmap | P4 (B2) bắt buộc | P4 **có điều kiện**: chỉ làm nếu test Android của bạn cho thấy B1 hỏng. B3 (file tay) vẫn làm, vì rẻ và dùng được cả từ laptop |

### 19.4 Chi tiết riêng cho Safari/iOS phải xử lý trong portal

| Vấn đề | Xử lý |
|---|---|
| **Cài đặt > Di động > Safari bị tắt** thì Safari không có 4G dù máy có | Hiện mục kiểm tra khi probe timeout: "Bật dữ liệu di động cho Safari" |
| iCloud Private Relay / "Giới hạn theo dõi IP" | Không ảnh hưởng: địa chỉ mạng nội bộ không đi qua relay; request GitHub vẫn đi qua 4G |
| Safari tự thử nâng `http` lên `https` | 192.168.4.1:443 bị từ chối ngay → Safari quay về http, không treo. Ghi chú để test |
| Khóa màn hình / chuyển app thì JS bị tạm dừng | Transfer chỉ 10–40 s, nên cần hiện "Giữ màn hình mở"; resume theo offset lo phần rủi ro còn lại. Screen Wake Lock API **không có trên http** (cần secure context) → chỉ nhắc bằng chữ |
| `viethud.local` | iOS phân giải mDNS sẵn, nên dùng được làm tên phụ. QR vẫn dùng IP vì Android không chắc phân giải được |
| Kiểm tra nhanh "router" trên iPhone | *Cài đặt > Wi-Fi > (i) VietHUD-XXXX*: ô **Bộ định tuyến** phải **trống** khi dùng profile Bridge |
| Tốc độ | SoftAP 2.4 GHz → iPhone thường đạt vài Mbit/s trở lên; nút thắt là ghi SD 1-bit — đo ở test 2 |

### 19.5 Test làm được NGAY với firmware hiện tại (không cần flash, khoảng 5 phút)

Mục đích: biết iPhone của bạn xử lý trường hợp **xấu nhất** (captive + có router) ra sao.

1. iPhone: *Cài đặt > Di động > Safari* = BẬT. Tắt Wi-Fi nhà để tránh tự nối.
2. VietHUD: Settings > WiFi (màn QR) → dùng Camera quét QR để nối `VietHUD-XXXX`.
3. Nếu màn CNA hiện ra: bấm **Hủy** → chọn **Dùng không có Internet** (nếu iOS hỏi).
4. Mở **Safari**, tab 1: `http://192.168.4.1` → trang Live phải hiện ra (đi qua Wi-Fi).
5. Tab 2: `https://raw.githubusercontent.com/911273/VietHUD/main/speedmap/manifest.txt`
6. Ghi lại *Cài đặt > Wi-Fi > (i)*: ô Bộ định tuyến đang hiện gì.

| Tab 1 | Tab 2 | Ý nghĩa |
|---|---|---|
| ✓ | ✓ thấy dòng `version 2026.09.26.0221` | iPhone giữ 4G làm primary **ngay cả với captive** → B1 chạy trên máy này; profile Bridge chỉ còn để làm chắc thêm |
| ✓ | ✗ lỗi ngay / trang không mở được | Wi-Fi đang là primary (do router + captive) → cần profile Bridge (test 2) |
| ✗ | ✓ | Hiếm; do đang ở CNA hoặc Wi-Fi đã rớt → làm lại từ Safari |

(CORS của raw đã kiểm bằng curl nên hai tab riêng là đủ đại diện: định tuyến theo địa chỉ đích, không theo tab.)

### 19.6 Test 2 — firmware thử với profile Bridge (thay đổi khoảng 30 dòng, chỉ trong `applyWifiState`, có cờ bật/tắt)

- Tắt `OFFER_ROUTER`, DNS không wildcard, bỏ các 302 captive; thêm trang `/probe`. Trang này tự làm 3 việc trong **cùng một trang**: fetch `/api/status` (local), fetch raw GitHub (4G), rồi PUT thử 2 MB dữ liệu rác xuống SD. Báo lại độ trễ và KB/s của từng việc.
- Kiểm tra: ô Bộ định tuyến trống; `/probe` báo cả ba ✓; lặp lại khi tắt 4G (local vẫn ✓, GitHub ✗ nhưng không làm hỏng trang).
- Lặp lại trên máy Android (Honor Magic V3) để quyết định có cần B2 hay không.
- Hai test này thay cho toàn bộ Phase 0 cũ và rút Phase 0 xuống khoảng 0.5 ngày.

### 19.7 KẾT QUẢ test 1 (2026-09-26 13:00, iPhone 14 Pro Max, iOS 26.2, firmware hiện tại)

| Quan sát | Giá trị | Ý nghĩa |
|---|---|---|
| Ô Router trên iPhone | `192.168.4.1` | Xác nhận DHCP của firmware hiện tại **có** cấp router (đúng dự đoán §19.2) |
| Thanh trạng thái khi đang nối `VietHUD-43F0` | hiện **"4G"**, không hiện biểu tượng Wi-Fi | iOS **không** chọn Wi-Fi VietHUD làm primary — 4G vẫn là đường Internet |
| `http://192.168.4.1` | Trang Live tải được, AP clients = 1 — **nhưng mở trong sheet "Captive Wi-Fi" (CNA)**, chưa phải Safari | Local chạy |
| `raw.githubusercontent.com/.../manifest.txt` | Hiện đủ `version 2026.09.26.0221` + 7 dòng | Internet qua 4G chạy **trong khi vẫn nối AP** |

**Kết luận:** giả thuyết của người dùng **đúng** trên iOS 26.2 — iPhone giữ 4G làm primary ngay cả khi AP có router, ít nhất ở trạng thái "captive chưa đăng nhập". B1 được **xác nhận khả thi** trên iPhone.

Còn phải kiểm (test 1b, không cần flash): đóng sheet CNA (chọn "Dùng không có Internet"), rồi mở **cả hai URL trong Safari**, xác nhận thanh trạng thái vẫn "4G". Lý do: bản thân CNA chỉ đi qua Wi-Fi nên **không được** chạy cập nhật trong đó; và cần biết iOS có đổi primary sang Wi-Fi sau khi đóng CNA hay không. Kết quả 1b sẽ chốt profile: nếu vẫn "4G" thì có thể giữ captive (có popup tự mở) cho phần cấu hình, chỉ cần trang trong CNA hướng người dùng mở Safari để cập nhật; nếu đổi sang Wi-Fi thì chuyển sang profile Bridge (§19.3).

Phụ: ảnh Live cho thấy STA đang "dang ket noi \"VuPQ\"..." trong lúc test → `wmTick()` đang quét/nối lại (điểm C2). Nó không làm hỏng test này, nhưng phải chặn khi có phiên upload.

---

## 20. Trạng thái triển khai (2026-09-26, firmware 2.1.0, branch `feature/update-bridge`)

Test 1b trên iPhone (người dùng xác nhận): mở portal bằng Safari sau khi đóng cửa sổ Captive → vẫn dùng 4G ⇒ **giữ captive profile** (popup tự mở cho phần cấu hình), cập nhật chạy trong Safari; không cần đổi DHCP.

### Đã làm

| Hạng mục | File |
|---|---|
| Staging + chữ ký ECDSA P-256 + commit journal + cài lúc boot + rollback (mount lỗi / 3 lần boot không xác nhận) + xác nhận sau 30 s | `src/update/DataInstaller.{h,cpp}` |
| API `/api/v1/{ping, update/state, update/session (POST/GET/DELETE), update/file (PUT raw), update/commit, update/direct}` + CSRF header `X-VietHUD` | `src/net/UpdateApi.{h,cpp}` |
| Portal SPA mới (Dữ liệu / Trạng thái / Cài đặt / Wi-Fi / Hệ thống), SHA-256 JS, upload resumable, B3 chọn tệp | `src/net/PortalPage.h` |
| Mode A dùng chung installer (chữ ký, Range resume, staging) + màn update-mode tiếng Việt có thanh tiến độ, thử lần lượt mọi mạng đã lưu | `src/net/DataUpdater.cpp`, `src/main_viethud.cpp` |
| SD: writer giữ handle, gom ghi 32 KB PSRAM, readback SHA, free space, clear dir | `src/map/SdCardManager.cpp` |
| C2/C6: tạm dừng wmTick + scan + auto-off khi đang cập nhật; WDT feed trong raw handler; webTask stack 8→12 KB; `WiFi.setSleep(false)` | `src/net/WebPortal.cpp` |
| Mật khẩu AP ngẫu nhiên thay `12345678` (migration cfgVer 3) | `src/core/NvsStore.cpp` |
| Màn QR 2 mã (vào Wi-Fi / mở trang) + trạng thái nhận dữ liệu + thanh tiến độ; nhãn tiến độ nổi trên Dashboard; màn "ĐÃ CẬP NHẬT / KHÔI PHỤC DỮ LIỆU CŨ" lúc boot | `src/ui/Settings.cpp`, `src/main_viethud.cpp` |
| Firmware OTA rollback (`verifyRollbackLater` + mark valid sau 30 s) | `src/main_viethud.cpp` |
| Ký manifest: `tools/sign_manifest.py`; pipeline OSM tự ký; `speedmap/manifest.txt.sig` đã publish | `tools/`, GitHub `main` |

### Phát hiện khi test trên thiết bị thật (đã sửa)

- **Mỗi request upload tốn cố định ~5 s**: WebServer đọc body bằng `readBytes(buf,1436)` và chờ hết timeout 5 s cho phần đuôi < 1436 B. Sửa: client gửi chunk bội số 1436 B; thiết bị hạ timeout đọc còn 800 ms (`PortalServer::setCurrentReadTimeoutMs`). 64 KB: 5,4 s → 0,33 s.
- **Modem sleep STA** làm RTT 27–191 ms → `WiFi.setSleep(false)` khi Wi-Fi bật (RTT còn ~9 ms).
- Ghi SD từng mảnh 1,4 KB → gom 32 KB.
- Sau rollback+reboot không hiện thông báo → cờ NVS `rbShow`.

### Kết quả test (PC đóng vai điện thoại qua hotspot 2.4 GHz; JS portal thật chạy trong Node)

| Ca | Kết quả |
|---|---|
| Không header CSRF / chữ ký sai / manifest bị sửa | 403 / 401 / 401 ✓ |
| Tệp đúng kích thước nhưng sai nội dung | commit 422, part bị xoá, gửi lại ✓ |
| Rớt kết nối giữa chunk (đóng socket ở 50 KB) | giữ 312 144 B, resume đúng offset; offset cũ → 409 ✓ |
| Cài qua journal + reboot + Wi-Fi tự bật lại + portal báo hoàn tất | ✓ (2 MB: 45 s tổng, gồm reboot) |
| `metadata.bin` hỏng nhưng ký hợp lệ | mount lỗi → tự rollback → dữ liệu gốc, màn "KHÔI PHỤC" ✓ |
| Manifest mới, dữ liệu giống hệt | adopt manifest, không truyền gì ✓ |
| Mode A (VietHUD tự tải qua Wi-Fi) | update-mode → chữ ký → tải 1,9 MB/33 s → cài lúc boot ✓ |
| B3 (chọn tệp, không Internet) | ✓ |
| Điện thoại không có Internet | "Không có Internet — cấu hình vẫn dùng bình thường", không lỗi ✓ |
| Crash / reset ngoài ý muốn trong toàn bộ phiên test | 0 |

Thông lượng upload (qua STA/hotspot PC): ~170–220 KB/s mạng, ~110 KB/s tính cả ghi SD. Qua AP trực tiếp với điện thoại dự kiến ≥ mức này.

### Còn lại / lưu ý vận hành

- **Mọi lần phát hành `manifest.txt` phải ký** (`tools/sign_manifest.py`). Job phát hành tự động trên Pi (`/home/admin/viethud-pipeline`, 02:00 hằng ngày) hiện chưa ký ⇒ sau lần chạy tới, VietHUD sẽ từ chối bản mới ("Chữ ký dữ liệu không hợp lệ") cho tới khi manifest được ký lại. Pipeline OSM/ODbL (`tools/viethud-pipeline`) đã tự ký.
- Chưa làm: chữ ký cho firmware OTA (`/update` vẫn nhận `.bin` không ký — đã có rollback), gói `.vhpkg` một tệp (B3 hiện chọn nhiều tệp), B2 (không cần vì iPhone chạy B1).
- Chưa test trên Android thật.
