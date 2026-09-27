# VietHUD V3 — firmware 3.0.0

Ngày phát hành: 27/09/2026 · Thiết bị: ESP32-S3 (JC3248W535, N16R8) · Nhánh: `main` · Tag: `v3`

V3 tập trung vào giao diện: toàn bộ chữ trên thiết bị chuyển sang tiếng Anh, theme Sáng/Tối hoạt động thật, bản đồ mờ dần về hai bên, biển tốc độ lớn hơn, tên đường dài tự chạy chữ, và bỏ chế độ demo. Phần cảnh báo, giới hạn tốc độ theo luật và các sửa lỗi ổn định của 2.2.0 được giữ nguyên — xem [VIETHUD_FW_2.2.0.md](VIETHUD_FW_2.2.0.md).

---

## 1. Thay đổi so với 2.2.0

### 1.1 Giao diện tiếng Anh

- Mọi chữ hiển thị trên màn hình thiết bị là tiếng Anh: các tab Setting, thông báo bật/tắt âm thanh, màn hình kết nối điện thoại (QR), màn hình cập nhật và khôi phục dữ liệu, các thông báo lỗi khi cập nhật.
- Tên đường giữ nguyên tiếng Việt.
- Tab âm thanh trong Setting tên là **Audio**.
- Trang web cài đặt mở trên điện thoại vẫn là tiếng Việt; một số dòng trạng thái cập nhật dữ liệu trong trang hiện tiếng Anh.

### 1.2 Theme Auto / Light / Dark

Ở các bản trước, lựa chọn Theme không có tác dụng: màn hình luôn dùng nền đen.

| Theme | Hiển thị |
|---|---|
| **Light** | Nền bản đồ xám sáng, đường màu đậm, chữ đen, thẻ cảnh báo và ô tên đường nền trắng — dễ đọc dưới nắng |
| **Dark** | Nền đen, đường xám, chữ trắng (như các bản trước) |
| **Auto** | Ban ngày Light, ban đêm Dark, theo giờ mặt trời mọc/lặn tính từ GPS. Khi chưa có giờ từ GPS: Dark |

Chọn Theme (và các lựa chọn dạng nút khác trong Setting) được lưu ngay, không cần bấm Save.

### 1.3 Bản đồ mờ dần về hai bên

Đường trên bản đồ mờ dần trong 100 px sát mép trái và mép phải màn hình (80 px ở mép trên và dưới). Hiệu ứng được vẽ trực tiếp vào bản đồ nên chuyển mượt ở cả hai theme; lớp phủ làm mờ cũ bị bỏ vì trên màn hình RGB565 nó chỉ tạo ra một dải phẳng có mép cứng.

### 1.4 Biển giới hạn tốc độ lớn hơn

Đường kính 88 → 104 px, viền đỏ 11 px. Số hai chữ số hiển thị cỡ 48 px; số ba chữ số (100, 120) cỡ 36 px để vừa trong vòng.

### 1.5 Tên đường dài tự chạy chữ

Tên đường dài hơn ô hiển thị chạy liên tục từ phải qua trái (khoảng 43 px/giây) rồi lặp lại, để đọc được toàn bộ tên. Tên ngắn vừa ô thì đứng yên.

### 1.6 Bỏ chế độ demo

Chế độ demo (kịch bản giả lập các trạng thái trên màn hình) đã bị xoá: không còn công tắc "Demo mode" trong Setting, lệnh serial `d` và nút bật/tắt demo trên trang web. Việc kiểm thử dùng mô phỏng GPS chạy qua dữ liệu thẻ nhớ thật (lệnh `S`, `W` — mục 5).

### 1.7 Sửa lỗi giao diện Setting

- Các công tắc ở cột phải không còn bị cắt ở mép màn hình; nút xoay màn hình "270" không còn tràn ra ngoài.
- Tab Map hiển thị đúng giới hạn tốc độ và độ tin cậy (trước đây hiện "f km/h" và "(f)").

---

## 2. Thao tác trên màn hình chính

| Thao tác | Tác dụng |
|---|---|
| Chạm nhanh | Đổi mức zoom bản đồ (1,5× → 2,0× → 2,5×) |
| Giữ 1–2,5 giây rồi thả | Mở Setting |
| Giữ ≥ 2,5 giây | Bật/tắt âm thanh cảnh báo |

## 3. Các tab Setting

| Tab | Nội dung |
|---|---|
| Display | Brightness, Brightness mode (Auto/Manual), Dim after stopped, Theme (Auto/Light/Dark), Rotation |
| Map | Heading up, Vehicle trail, trạng thái Speed Map (Status, Region, Version, Current limit, Source, Match, Road ID) |
| Sensors | Trip logging, trạng thái GNSS, hiệu chỉnh GNSS, Overspeed offset, Default limit |
| WiFi | Màn hình QR để điện thoại kết nối; công tắc Wi-Fi (mở màn hình này không tự bật Wi-Fi) |
| Audio | Alert sound, Volume, và chọn loại cảnh báo có âm thanh: Overspeed, Camera, Speed limit ahead, Residential area, No overtaking, Toll booth, Traffic light, Danger zone, GPS / temperature |

---

## 4. Nạp firmware

Các file nằm trong [`firmware/3.0.0/`](../firmware/3.0.0/); kiểm tra bằng `SHA256SUMS.txt`.

| File | Địa chỉ | SHA-256 |
|---|---|---|
| `bootloader.bin` | `0x0` | `2a71d69b471e20c2bac7fb469f3c6a807b3ebee780e348e5889db0da849ca363` |
| `partitions.bin` | `0x8000` | `bd0f7954aca2ef7d925ee21aaa1f3dc8822d1d6ce5cbbd26a135e5886bfff6ce` |
| `boot_app0.bin` | `0xe000` | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` |
| `firmware.bin` | `0x10000` | `b885a4734edb7f2252891de3a7843c26bc4183b00a6e41bde0b167cec6863949` |

Bootloader và bảng phân vùng giống hệt bản 2.2.0, nên từ 2.x có thể cập nhật chỉ bằng `firmware.bin`.

**Cập nhật qua trang web của thiết bị (giữ nguyên cài đặt):** bật Wi-Fi trên thiết bị, kết nối điện thoại, mở trang cài đặt (192.168.4.1), mục **Hệ thống** → cập nhật firmware, chọn `firmware.bin`.

**Nạp qua USB:**

```bash
esptool.py --chip esp32s3 --baud 921600 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Hoặc từ mã nguồn: `python -m platformio run -e viethud -t upload`.

---

## 5. Kiểm thử

- Máy tính: `test/host/run.sh` 46/46 đạt; `test/host/run_track.sh` đạt toàn bộ.
- Trên thiết bị, với dữ liệu thẻ nhớ thật và mô phỏng GPS, kiểm tra bằng ảnh chụp màn hình qua serial:
  - Theme: chuyển Light → Dark → Auto đúng; cài đặt Light giữ nguyên sau khi reset phần cứng.
  - Bản đồ mờ dần về hai bên ở cả Light và Dark, không còn dải phẳng.
  - Biển tốc độ 104 px hiển thị đúng.
  - Tên đường "Cầu vượt nút giao Chùa Bộc - Phạm Ngọc Thạch" chạy chữ từ phải qua trái.
  - Tất cả tab Setting hiển thị tiếng Anh, không bị cắt chữ.
  - Chạy tuyến có biển tốc độ và cảnh báo: hoạt động bình thường, không crash.
- Các kiểm thử trên thiết bị chạy với mã nguồn của commit `e06f3bf`; bản dựng 3.0.0 chỉ khác ở chuỗi phiên bản.

### Lệnh serial cho kiểm thử (115200 baud)

| Lệnh | Tác dụng |
|---|---|
| `S lat lon hướng kmh giây` | Mô phỏng xe chạy thẳng |
| `W kmh lat lon lat lon …` | Mô phỏng xe chạy theo tuyến (tối đa 48 điểm) |
| `P` | Chụp màn hình: gửi ảnh RGB565 thô qua serial (`[snap] w h stride`, dữ liệu, `[snap] end`) |
| `a` | Phát thử toàn bộ âm thanh |
| `w` | Bật/tắt Wi-Fi |
| `T x y` / `H x y ms` / `G x1 y1 x2 y2 ms` | Giả lập chạm / giữ / kéo |
| `u` | Cập nhật dữ liệu online |

---

## 6. Hạn chế đã biết

- Chưa chạy thử trên xe thật với V3.
- Trang web cài đặt trên điện thoại vẫn là tiếng Việt.
- Dữ liệu biển báo/camera hiện tại (nguồn WYN) lệch toạ độ so với bản đồ đường, nên chỉ khoảng 6% cảnh báo có thể xuất hiện trên đường thật — xem mục 6 trong [VIETHUD_FW_2.2.0.md](VIETHUD_FW_2.2.0.md).
