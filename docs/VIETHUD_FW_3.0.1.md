# VietHUD firmware 3.0.1

Ngày phát hành: 27/09/2026 · Tag: `v3.0.1` · Bản trước: [3.0.0](VIETHUD_FW_3.0.0.md)

## 1. Thay đổi

| # | Thay đổi |
|---|---|
| 1 | **Zoom bản đồ mặc định 2×** khi khởi động. Chạm nhanh vẫn đổi zoom: 2,0× → 2,5× → 1,5× → 2,0×. |
| 2 | **Màn hình Setting > WiFi tự quay về màn hình chính sau 1 phút** không chạm. Không tự đóng khi điện thoại đang gửi dữ liệu cho thiết bị. Trạng thái Wi-Fi giữ nguyên khi đóng. |
| 3 | **Bổ sung đèn tín hiệu giao thông từ OpenStreetMap**, tích hợp sẵn trong firmware (7.898 đèn trên toàn quốc, 820 trong nội thành Hà Nội). Khi khởi động, firmware gộp các đèn này với dữ liệu cảnh báo trên thẻ nhớ và bỏ qua những đèn thẻ nhớ đã có trong vòng 30 m. 4.514 đèn có hướng áp dụng (thẻ OSM `traffic_signals:direction`) nên chỉ cảnh báo khi xe đi đúng hướng đó; các đèn còn lại cảnh báo theo mọi hướng tới. |

## 2. Kiểm tra dữ liệu cảnh báo ở nội thành Hà Nội

Vùng kiểm tra: vĩ độ 20,98–21,07, kinh độ 105,78–105,88 (Hoàn Kiếm, Ba Đình, Đống Đa, Hai Bà Trưng, Cầu Giấy và lân cận).

| | Dữ liệu trên thẻ nhớ (`VietHUD_SDCard_Ready`) | Dữ liệu OTA trên GitHub (`speedmap/`, 2026.09.27.0200) | OpenStreetMap (toạ độ thực) |
|---|---|---|---|
| Đèn tín hiệu | **0** | **0** | 820 điểm = 364 nút giao có đèn |
| Camera | **0** (8.075 trên toàn quốc) | **33** (đều nằm trên đường) | 0 |

Kết luận:

- **Thiếu cảnh báo đèn tín hiệu** là do dữ liệu WYN không có đèn nào ở nội thành Hà Nội, không phải lỗi firmware. Firmware 3.0.1 bổ sung đèn từ OpenStreetMap (mục 1, thay đổi số 3).
- **Thiếu camera phạt nguội**: bộ dữ liệu trên thẻ nhớ không có camera nào ở nội thành Hà Nội; bộ dữ liệu OTA mới hơn có 33. OpenStreetMap không có dữ liệu camera ở khu vực này, nên firmware không thể tự bổ sung. Cần nguồn dữ liệu camera khác (ví dụ tự khảo sát) để khắc phục.
- **Lưu ý trước khi cập nhật dữ liệu online:** bộ dữ liệu OTA hiện tại trên GitHub (2026.09.27.0200) có bản đồ đường **chỉ gồm Hà Nội** (vùng `VN-HN`, 3.437 ô), trong khi thẻ nhớ đang dùng bản đồ toàn quốc (175.666 ô). Cập nhật dữ liệu online sẽ thay bản đồ toàn quốc bằng bản đồ Hà Nội.

## 3. Cập nhật firmware

Các file trong [`firmware/3.0.1/`](../firmware/3.0.1/); bootloader và bảng phân vùng giống bản 3.0.0 / 2.2.0.

| File | Địa chỉ | SHA-256 |
|---|---|---|
| `bootloader.bin` | `0x0` | `2a71d69b471e20c2bac7fb469f3c6a807b3ebee780e348e5889db0da849ca363` |
| `partitions.bin` | `0x8000` | `bd0f7954aca2ef7d925ee21aaa1f3dc8822d1d6ce5cbbd26a135e5886bfff6ce` |
| `boot_app0.bin` | `0xe000` | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` |
| `firmware.bin` | `0x10000` | `f6fa4b8276ce38f26f1262516501d8280ef05965239d24d4593d40a0f2dc74bf` |

**OTA qua trang web của thiết bị:** bật Wi-Fi trên thiết bị, kết nối điện thoại, mở 192.168.4.1, mục **Hệ thống** → cập nhật firmware, chọn `firmware.bin`.

Sau khi cập nhật, log serial khi khởi động có dòng `[sdmgr] built-in OSM traffic lights: N added, M already on the card` xác nhận đèn đã được gộp.

## 4. Kiểm thử

- Máy tính: `test/host/run.sh` 46/46, `test/host/run_track.sh` đạt toàn bộ, `test/host/run_signmerge.sh` (gộp đèn OSM: loại trùng 30 m, giữ hướng, giới hạn dung lượng, thẻ nhớ không có biển báo) 10/10.
- Hướng của đèn OSM: kiểm tra 600 đèn có hướng, 93,6% khớp với chiều đi hợp lệ của con đường đèn nằm trên.
- **Chưa chạy trên thiết bị**: bản 3.0.1 được build khi không có board. Trên thiết bị chỉ đã kiểm tra mã nguồn của 3.0.0.

## 5. Nguồn dữ liệu

Đèn tín hiệu tích hợp trong firmware: © OpenStreetMap contributors, giấy phép ODbL 1.0 (https://www.openstreetmap.org/copyright). Tạo lại bằng `tools/map_builder/build_osm_signals.py` từ `vietnam-latest.osm.pbf`.
