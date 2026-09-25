# 🚗 VietHUD - Dữ Liệu Giao Thông & Bản Đồ Ngoại Tuyến (Offline Traffic Data)

> Kho lưu trữ chính thức đồng bộ dữ liệu giao thông, camera phạt nguội và biển báo cho thiết bị **VietHUD**.

**Phiên bản phát hành (Version):** `2026.09.25.1116`  
**Thời gian cập nhật:** `2026-09-25 11:16:04`  
**Đường dẫn OTA tải về (VietHUD OTA URL):**  
```text
https://raw.githubusercontent.com/911273/VietHUD/main/speedmap/
```

---

## 📦 Danh Sách Dữ Liệu Đồng Bộ (Core SpeedMap Files)

| File | Kích thước | Mã băm SHA-256 | Mô tả |
| :--- | :--- | :--- | :--- |
| `cameras.bin` | 1.15 MB | `b8f4538e1659a3d7...` | Dữ liệu vị trí camera phạt nguội & camera bắn tốc độ toàn quốc |
| `signs.bin` | 1.78 MB | `151146e506f582e1...` | Dữ liệu biển báo giao thông (khu dân cư, cấm vượt, trạm thu phí, ...) |
| `tiles.bin` | 4.87 MB | `ff41ca79e36916ca...` | Mạng lưới đường bộ vector & giới hạn tốc độ chi tiết |
| `index.bin` | 94.0 KB | `25656be57c8a9b9b...` | Lưới chỉ mục không gian (Spatial Index) tra cứu nhanh |
| `metadata.bin` | 0.1 KB | `f6beebdf94259322...` | Thông số khung tọa độ & cấu hình bản đồ |
| `names.bin` | 141.2 KB | `6edd2dc0859dc9b5...` | Từ điển tên đường phố toàn quốc |
| `seg_names.bin` | 355.9 KB | `d210aeeece6f4213...` | Ánh xạ định danh phân đoạn đường sang tên phố |

---

## 🛠 Hướng Dẫn Cấu Hình Thiết Bị VietHUD Cập Nhật OTA
1. Trên thiết bị VietHUD, vào mục **Cài đặt (Settings) -> WiFi** (hoặc truy cập WebPortal tại `http://192.168.4.1` khi kết nối WiFi phát từ VietHUD).
2. Tại ô **Data Update URL**, điền chính xác đường dẫn sau:
   ```text
   https://raw.githubusercontent.com/911273/VietHUD/main/speedmap/
   ```
3. Kết nối VietHUD vào Hotspot Wi-Fi của điện thoại.
4. Bấm **Kiểm tra cập nhật (Check Update)** trên WebPortal hoặc màn hình HUD. Thiết bị sẽ tự động tải các file thay đổi, ghi vào thẻ nhớ MicroSD và khởi động lại.
