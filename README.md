# 🚗 VietHUD - Standalone Offline Speed & Traffic Alert HUD

GPS-only offline speed-limit / speed-camera / traffic-sign warning device —
**ESP32-S3 (JC3248W535)** + **u-blox M10N GNSS** + an offline speed-map database
built from OpenStreetMap + a WYN traffic-signs/road-network export, loaded
from a microSD card at boot.

No radar, no cellular/SIM card, no cloud dependency needed while driving — everything the device warns about comes from real-time GNSS positioning matched against binary data stored on the MicroSD card.

---

## 🌟 What it does

- **Real-Time GNSS Positioning:** Reads position, speed, and heading from a u-blox M10N module via UART2 at 5Hz.
- **Offline Map Matching & Hazard Lookahead:** Matches current position against a vector speed-map database (`speedmap/*.bin`) to display:
  - Current road speed limit
  - Upcoming speed-limit changes
  - Speed cameras / traffic enforcement cameras
  - Resident area entry/exit (R.420 / R.421)
  - No-overtaking zones (P.125 / DP.133)
  - Toll booths (P.135)
  - Traffic lights & intersection cameras
- **Dynamic Lookahead Distance:** Automatically calculates warning distance based on vehicle velocity:
  $$D_{warn} = \text{clamp}(v_{kmh} \times 3.0\text{ m}, 150\text{m}, 500\text{m})$$
- **Rich LVGL Dashboard (30 FPS):** Dual-orientation HUD (Landscape & Portrait) with large high-contrast 7-segment digital speedometer and dynamic hazard countdown alert cards.
- **Vietnamese Voice Guidance:** Real-time I2S audio announcements (`NS4168` amp + speaker) queue MP3 voice lines in Vietnamese without stutter or UI frame drops.
- **Wi-Fi WebPortal & Cloud OTA:** Serves live telemetry, trip logs, configuration, and **wireless GitHub OTA data updates** (`src/net/DataUpdater.cpp` and `src/net/WebPortal.cpp`).

---

## 📦 Dữ Liệu Đồng Bộ OTA Qua GitHub (SpeedMap OTA Files)

> **Kho lưu trữ chính thức:** [https://github.com/911273/VietHUD.git](https://github.com/911273/VietHUD.git)  
> **Đường dẫn OTA tải về thiết bị (VietHUD OTA URL):**  
> ```text
> https://raw.githubusercontent.com/911273/VietHUD/main/speedmap/
> ```

| File | Kích thước | Mô tả |
| :--- | :--- | :--- |
| `cameras.bin` | ~1.15 MB | Vị trí camera phạt nguội & camera tốc độ toàn quốc |
| `signs.bin` | ~1.78 MB | Biển báo khu dân cư, cấm vượt, trạm thu phí |
| `tiles.bin` | ~4.87 MB | Mạng lưới đường bộ vector & giới hạn tốc độ chi tiết |
| `index.bin` | ~94.0 KB | Lưới chỉ mục không gian tra cứu nhanh |
| `metadata.bin` | ~0.1 KB | Thông số khung tọa độ & cấu hình bản đồ |
| `names.bin` | ~141.2 KB | Từ điển tên đường phố toàn quốc |
| `seg_names.bin` | ~355.9 KB | Ánh xạ định danh phân đoạn đường sang tên phố |
| `sounds/` | ~3.0 MB | Thư viện file âm thanh cảnh báo MP3 tiếng Việt |

### Cách cập nhật dữ liệu OTA trên thiết bị:
1. Kết nối VietHUD vào Hotspot Wi-Fi của điện thoại (hoặc Wi-Fi nhà).
2. Dùng điện thoại truy cập WebPortal tại `http://192.168.4.1` (hoặc `http://viethud.local`).
3. Dán URL trên vào ô **Data Update URL** và bấm **Lưu & Kiểm tra cập nhật**. Thiết bị sẽ tự động tải các file thay đổi, ghi vào thẻ nhớ và khởi động lại.

---

## 🛠 Hardware Specifications

- **Mainboard:** JC3248W535 (ESP32-S3-N16R8V — 16 MB Flash, 8 MB Octal PSRAM)
- **Display:** 3.5" AXS15231B QSPI LCD (480x320 / 320x480), I2C capacitive touch
- **GNSS Module:** u-blox M10N on UART2 (RX=GPIO 17, TX=GPIO 18)
- **Storage:** Dedicated onboard SD_MMC slot in 1-bit mode (CLK=12, CMD=11, D0=13)
- **Audio:** Onboard NS4168 I2S power amplifier (BCLK=42, LRCK=2, DOUT=41)
- **PlatformIO Board Definition:** `boards/esp32-s3-n16r8v.json`

---

## 🚀 Build & Flash

```bash
python -m platformio run -e viethud              # Biên dịch firmware chính
python -m platformio run -e viethud -t upload    # Nạp firmware vào board (COM3, 921600 baud)
python -m platformio device monitor -e viethud   # Xem log qua cổng Serial, 115200 baud
```

**Lưu ý quan trọng về Toolchain:**
`platform = espressif32@7.1.3` được ghim phiên bản cố định để tương thích với API `driver/i2s.h` của ESP-IDF 4.4 và Arduino-ESP32 2.0.17. Không nâng cấp tự do lên phiên bản 3.x/IDF 5.x để tránh xung đột thư viện âm thanh.

---

## 📂 Project Layout

```text
VietHUD/
├── .gitignore
├── README.md                     # Tài liệu tổng quan dự án & hướng dẫn
├── platformio.ini                # Cấu hình PlatformIO
├── boards/esp32-s3-n16r8v.json   # Định nghĩa board phần cứng JC3248W535
├── docs/                         # Tài liệu kỹ thuật chi tiết
│   ├── VIETHUD_DATA_AND_GITHUB_GUIDE.md
│   └── VIETHUD_WEBPORTAL_AND_GITHUB_GUIDE.md
├── include/                      # Header cấu hình phần cứng & đồ họa (pincfg.h, lv_conf.h)
├── lib/AXS15231B_Touch/          # Driver cảm ứng I2C cho panel AXS15231B
├── speedmap/                     # Dữ liệu bản đồ & camera phục vụ tải OTA
│   ├── manifest.txt
│   ├── cameras.bin, signs.bin, tiles.bin, ...
│   └── sounds/
├── src/                          # Toàn bộ mã nguồn firmware C++
│   ├── main_viethud.cpp          # Điểm khởi chạy chính của thiết bị
│   ├── core/                     # AppConfig, NVS, FreeRTOS SharedState
│   ├── gnss/                     # UART2 u-blox M10N & NMEA parser
│   ├── map/                      # SpeedLimitManager & SDCardManager
│   ├── ui/                       # Dashboard, Settings (LVGL 9.2.2), SevenSeg
│   ├── audio/                    # Bộ giải mã MP3 & Tone chimes I2S
│   ├── net/                      # WebPortal & DataUpdater (GitHub OTA)
│   ├── log/                      # TripLogger ghi dữ liệu ra thẻ nhớ
│   └── display/, touch/          # QSPI Display & Touch drivers
└── tools/                        # Bộ công cụ xử lý dữ liệu PC
    └── viethud_builder.py        # All-in-one builder & GitHub publisher
```
