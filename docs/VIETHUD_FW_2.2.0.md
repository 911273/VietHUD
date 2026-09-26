# VietHUD firmware 2.2.0

Ngày phát hành: 27/09/2026 · Thiết bị: ESP32-S3 (JC3248W535, N16R8) · Nhánh: `main`

Tài liệu này mô tả những gì thay đổi trong firmware 2.2.0, cách nạp, cách dùng các thao tác mới, kết quả kiểm thử trên thiết bị thật, và kết quả kiểm tra bộ dữ liệu đang dùng trên thẻ nhớ.

---

## 1. Tóm tắt

| Hạng mục | Trước (2.1.0) | Sau (2.2.0) |
|---|---|---|
| Khởi động với dữ liệu toàn quốc | Có thể khởi động lại liên tục (hết PSRAM) | Ổn định, còn ~2,7 MB PSRAM trống |
| Thời gian xử lý mỗi nhịp ở khu phố dày đặc | 0,4–3,5 giây | 40–130 ms |
| Vẽ bản đồ ở khu dày đặc, zoom rộng | ~950 ms mỗi lần cập nhật | ~6 ms |
| Số trên thẻ camera | Có thể là hướng la bàn (135, 178…) hoặc số đoán | Giới hạn thật của camera, hoặc chỉ hiện biểu tượng |
| Tốc độ sau khi qua biển thật | Bị số đoán theo loại đường ghi đè | Theo biển vừa qua |
| Biển "hết hạn chế" / khu dân cư | Không xử lý | Theo quy định (Thông tư 38/2024) hoặc theo dữ liệu đường |
| Bật/tắt âm thanh | Chỉ trong Setting | Giữ màn hình 2,5 giây + biểu tượng loa + chọn từng loại cảnh báo |
| Mở Setting > WiFi | Tự bật WiFi | Giữ nguyên trạng thái WiFi |

---

## 2. Thay đổi chi tiết

### 2.1 Cảnh báo theo lộ trình phía trước

- **Chọn nhánh ở ngã rẽ:** ưu tiên đi tiếp trên đường lớn, không tự đoán rẽ vào nhánh ra/vào cao tốc, không đổi tầng (cầu vượt ↔ đường dưới) nếu không có nhánh nối. Sửa lỗi hướng xe chỉ được lấy từ đoạn đầu tiên nên chọn sai nhánh sau khúc cua.
- **Chỗ đường tách nhánh:** biển báo và thay đổi tốc độ nằm sau điểm tách được giữ lại cho đến khi xe đã chọn nhánh. Camera vẫn báo ngay.
- **Camera/biển phải thuộc đúng đường đang đi:** cách tim đường ≤25 m và không có đường khác gần nó hơn rõ rệt (>3 m). Loại bỏ cảnh báo của đường gom song song, chiều ngược lại của đường có dải phân cách, đường dưới cầu vượt.

### 2.2 Giới hạn tốc độ đang áp dụng

Thứ tự ưu tiên của giới hạn hiển thị:

1. **Biển số tốc độ thật vừa đi qua** trên đường đang đi. Giữ nguyên cho đến khi xe rẽ sang đường không nằm trong lộ trình dự đoán, gặp biển khác, hoặc sau 20 phút.
2. **Biển "hết hạn chế tốc độ"** (biển tốc độ có giá trị 0 trong dữ liệu) và **biển bắt đầu/hết khu đông dân cư** (R.420/R.421): đoạn tiếp theo lấy
   - giới hạn thật của đường nếu dữ liệu có thẻ tốc độ OSM; nếu không thì
   - giới hạn theo luật, khi biết xe đang ở trong hay ngoài khu dân cư; nếu chưa biết thì
   - giá trị có sẵn của đoạn đường trong dữ liệu.
3. **Giới hạn của đoạn đường** trong dữ liệu (thẻ OSM, hoặc giá trị mặc định theo loại đường).

Giới hạn theo luật đang dùng (Thông tư 38/2024/TT-BGTVT, hiệu lực 01/01/2025, xe ô tô con và tải nhẹ, khi không có biển số tốc độ):

| | Đường đôi / một chiều ≥2 làn | Đường hai chiều / một chiều 1 làn |
|---|---|---|
| Trong khu đông dân cư | 60 km/h | 50 km/h |
| Ngoài khu đông dân cư | 90 km/h | 80 km/h |

"Đường đôi" được suy ra từ đoạn đường một chiều thuộc loại đường chính (bản đồ OSM vẽ đường có dải phân cách thành hai đường một chiều). Cao tốc luôn có biển riêng.

Các quy tắc khác:
- Biển "hết hạn chế" đứng trước một biển số trong vòng 50 m được coi là một lần thay đổi: áp dụng thẳng số trên biển, không hiện giá trị trung gian.
- Biển "hết hạn chế" ở phía trước làm thẻ "tốc độ phía trước" hiện giá trị sẽ áp dụng sau biển.
- Thông báo "tốc độ phía trước" dựa trên đoạn đường chỉ dùng các đoạn có thẻ tốc độ thật, không dùng số đoán.

### 2.3 Số trên thẻ camera

- Giới hạn của chính camera; nếu không có thì lấy từ bản trùng của cùng camera trong vòng 30 m (cùng hướng); nếu vẫn không có thì dùng giới hạn của đường chỉ khi đó là biển/thẻ thật. Ngoài các trường hợp trên chỉ hiện biểu tượng camera.
- Sửa lỗi công cụ tạo dữ liệu (`tools/viethud_builder.py`, `tools/map_builder/merge_vietmap_papago.py`) ghi ngược hai trường tốc độ/hướng của camera. Firmware tự nhận ra file bị ghi ngược và sửa khi đọc, nên thẻ nhớ tạo bằng công cụ cũ vẫn dùng được.

### 2.4 Âm thanh

- **Giữ màn hình chính 2,5 giây**: bật/tắt âm thanh cảnh báo. Vòng tiến trình đổi màu cam sau 1 giây; khi đủ 2,5 giây hiện thông báo "Âm thanh: BẬT/TẮT", có tiếng bíp khi bật lại.
- **Biểu tượng loa** bên trái đồng hồ (loa gạch màu đỏ khi tắt).
- **Tab Setting "Âm thanh"**: công tắc chính, âm lượng, và công tắc riêng cho từng loại: quá tốc độ, camera, đổi tốc độ phía trước, khu dân cư, cấm vượt, trạm thu phí, đèn tín hiệu, khu vực nguy hiểm, GPS/nhiệt độ.

### 2.5 WiFi

Mở Setting > WiFi không còn tự bật WiFi và đóng lại không tự tắt. Công tắc hiển thị đúng trạng thái hiện tại. Chế độ tự tắt sau 10 phút không kết nối vẫn giữ nguyên.

### 2.6 Ổn định và hiệu năng

- **Hết PSRAM với dữ liệu toàn quốc** (nguyên nhân khởi động lại liên tục): bảng chỉ mục ô bản đồ chỉ giữ các trường cần dùng (4,9 MB → 2,1 MB); camera/biển báo lưu gọn; bỏ bản sao camera trong `signs.bin`; luôn giữ 1,5 MB dự phòng cho giao diện.
- **Khu phố dày đặc:** tìm các đoạn đường tại một nút bằng tìm kiếm nhị phân thay vì duyệt 9 ô bản đồ; chọn 180 đường gần nhất để vẽ bằng heap; bộ nhớ đệm ô bản đồ tăng 12 → 24 ô.
- **Camera/biển báo** được sắp xếp theo vĩ độ khi nạp; mỗi lần tra cứu chỉ quét một dải hẹp.
- Sửa lỗi crash khi bấm nút "Defaults" trong Setting.

---

## 3. Thao tác nhanh trên màn hình chính

| Thao tác | Tác dụng |
|---|---|
| Chạm nhanh | Đổi mức zoom bản đồ (1,5× → 2,0× → 2,5×) |
| Giữ 1–2,5 giây rồi thả | Mở Setting |
| Giữ ≥2,5 giây | Bật/tắt âm thanh cảnh báo |

---

## 4. Nạp firmware

Các file nằm trong [`firmware/2.2.0/`](../firmware/2.2.0/). Kiểm tra checksum bằng `SHA256SUMS.txt`.

| File | Địa chỉ | SHA-256 |
|---|---|---|
| `bootloader.bin` | `0x0` | `2a71d69b471e20c2bac7fb469f3c6a807b3ebee780e348e5889db0da849ca363` |
| `partitions.bin` | `0x8000` | `bd0f7954aca2ef7d925ee21aaa1f3dc8822d1d6ce5cbbd26a135e5886bfff6ce` |
| `boot_app0.bin` | `0xe000` | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` |
| `firmware.bin` | `0x10000` | `53e5f6fa44d0dc2e3aaccb7b6620dcb2bd29bc0922dda2dbfce5be3681fb049b` |

**Cập nhật qua trang web của thiết bị (giữ nguyên cài đặt):** bật WiFi trên thiết bị, kết nối điện thoại/máy tính, mở trang cài đặt (192.168.4.1), mục **Hệ thống** → cập nhật firmware, chọn `firmware.bin`.

**Nạp qua USB (máy mới hoặc cần nạp lại toàn bộ):**

```bash
esptool.py --chip esp32s3 --baud 921600 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Hoặc từ mã nguồn: `python -m platformio run -e viethud -t upload`.

---

## 5. Kiểm thử

### 5.1 Kiểm thử trên máy tính

- `test/host/run.sh` (dự đoán lộ trình): 46/46 đạt.
- `test/host/run_track.sh` (nhận diện đường trên cao/dưới thấp): đạt toàn bộ.

### 5.2 Kiểm thử trên thiết bị với dữ liệu thẻ nhớ thật

Dùng lệnh serial `W` để mô phỏng xe chạy 40 km/h dọc theo đường OSM thật, tiến tới từng điểm cảnh báo và đi tiếp qua điểm đó. 17 tuyến trên nhiều tỉnh:

| # | Loại | Kết quả |
|---|---|---|
| 0–3 | Camera (50, 40, 60, không số) | Cả 4 cảnh báo từ ~170–180 m, đếm lùi khớp vị trí xe. Số hiển thị: 50, 40, 60, và 50 (lấy từ thẻ tốc độ OSM của đường) |
| 4–6 | Biển tốc độ (50, 80, 50) | Cảnh báo "tốc độ phía trước" đúng giá trị; sau khi qua biển giới hạn hiện đúng 50/80/50 |
| 7–8 | Đèn tín hiệu | Đúng; tuyến 8 có thêm đèn và trạm thu phí khác trên đường, cảnh báo lần lượt đúng thứ tự |
| 9–10 | Khu dân cư | Đúng |
| 11 | Trạm thu phí | Đúng (tuyến có thêm một camera, cũng được báo) |
| 12 | Khu vực nguy hiểm | Đúng |
| 13–14 | Biển hết hạn chế | Biển đứng riêng → áp dụng giá trị theo dữ liệu (50); biển hết hạn chế + biển 80 cách 32 m → chuyển thẳng lên 80 |
| 15 | Bắt đầu khu dân cư | Giới hạn chuyển 50 (luật, đường hai chiều) |
| 16 | Hết khu dân cư | Giới hạn chuyển 80 (luật, ngoài khu dân cư) |

Trong toàn bộ quá trình: không crash, không khởi động lại. Thiết bị chạy liên tục hơn 4 giờ (qua đêm). Bộ nhớ trong còn trống thấp nhất 128 KB, PSRAM còn 2,7 MB. Nhịp xử lý 40–130 ms khi đang chạy; các nhịp dài hơn chỉ xảy ra ngay khi mô phỏng "nhảy" tới một vùng mới và phải đọc nhiều ô bản đồ cùng lúc.

---

## 6. Kiểm tra bộ dữ liệu trên thẻ nhớ

Bộ dữ liệu trên thẻ trùng với thư mục `VietHUD_SDCard_Ready/`:
- Đường: OSM, 175.666 ô, 26,6 triệu đoạn.
- Biển báo và camera: nguồn WYN, 38.340 điểm (trong đó 8.075 camera), giống hệt file trong `VietHUD_SDCard_WYN_PURE/`.

Mỗi điểm được so với các đoạn đường gần nhất. Một điểm "có thể cảnh báo" nếu có đoạn đường trong vòng 25 m, đi đúng chiều cho phép, và lệch hướng với biển ≤60°.

| Loại | Số điểm | Có thể cảnh báo với đường OSM (trên thẻ) | So với mạng đường WYN |
|---|---|---|---|
| Biển tốc độ | 15.494 | 969 (6,3%) | 88,3% |
| Khu dân cư | 9.516 | 594 (6,2%) | 87,6% |
| Camera | 8.075 | 506 (6,3%) | 91,3% |
| Trạm thu phí | 3.050 | 145 (4,8%) | 72,9% |
| Đèn tín hiệu | 1.433 | 74 (5,2%) | 64,8% |
| Khu vực nguy hiểm | 772 | 37 (4,8%) | 82,5% |

**Kết luận:** tọa độ biển báo và camera khớp với mạng đường WYN (khoảng cách trung vị 2,5 m) nhưng không khớp với đường OSM trên thẻ (trung vị khoảng 1 km). Có 15.588 điểm nằm ở nơi không có đường OSM nào; ví dụ điểm (8,179; 104,990) nằm ngoài biển, cách Mũi Cà Mau khoảng 45 km, trong khi dữ liệu WYN vẫn có "đường" tại đó. Tọa độ của dữ liệu WYN không phải tọa độ GPS thực tế.

Hệ quả: với GPS thật, chỉ khoảng 6% cảnh báo trong bộ dữ liệu hiện tại có thể xuất hiện, và một số ít trong đó có thể trùng nhầm vào đường khác.

**Khuyến nghị:** dùng dữ liệu camera/biển báo có tọa độ thực tế cùng hệ với bản đồ đường, ví dụ camera, đèn tín hiệu và biển tốc độ lấy từ OpenStreetMap, hoặc dữ liệu tự khảo sát qua pipeline trên Raspberry Pi.

Các chỉ số khác:
- 5.221 biển tốc độ có giá trị 0 (firmware coi là biển hết hạn chế).
- 6.372/8.075 camera không có giới hạn tốc độ.
- 188 cặp điểm trùng (cùng loại, cách nhau ≤10 m, cùng hướng).
- Một số "trạm thu phí" nằm trên đường làng, nhiều khả năng bị gán sai loại trong dữ liệu gốc.

---

## 7. Lệnh serial cho kiểm thử (115200 baud)

| Lệnh | Tác dụng |
|---|---|
| `S lat lon hướng kmh giây` | Mô phỏng xe chạy thẳng |
| `W kmh lat lon lat lon …` | Mô phỏng xe chạy theo tuyến (tối đa 48 điểm) |
| `d` | Bật/tắt chế độ demo |
| `a` | Phát thử toàn bộ âm thanh |
| `w` | Bật/tắt WiFi |
| `T x y` / `H x y ms` / `G x1 y1 x2 y2 ms` | Giả lập chạm / giữ / kéo |
| `u` | Cập nhật dữ liệu online |

Log liên quan: `[warn]` mỗi giây khi xe chạy (giới hạn và nguồn, camera, biển kế tiếp, tốc độ phía trước); `[map] tick max …` mỗi 3 giây (thời gian xử lý, số lần đọc ô bản đồ); `[map] … -> current limit …` khi giới hạn đổi theo biển.

---

## 8. Hạn chế đã biết

- Chưa chạy thử trên xe thật với firmware 2.2.0; toàn bộ kiểm thử thiết bị ở trên dùng mô phỏng GPS với dữ liệu thẻ nhớ thật.
- "Đường đôi" trong bảng giới hạn theo luật được suy ra từ dữ liệu bản đồ, không biết số làn thực tế.
- Trạng thái trong/ngoài khu dân cư chỉ biết được sau khi đi qua biển R.420/R.421; trước đó dùng giá trị của dữ liệu đường.
- Trang web cài đặt chưa có các công tắc âm thanh theo từng loại cảnh báo.
