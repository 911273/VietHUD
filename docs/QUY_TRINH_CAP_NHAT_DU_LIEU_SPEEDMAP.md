# QUY TRÌNH CHUẨN: CẬP NHẬT DỮ LIỆU BẢN ĐỒ, CAMERA & BIỂN BÁO VIETHUD (BẢN V3)

Tài liệu này là **bộ quy tắc ghi nhớ và cẩm nang vận hành chuẩn** khi có dữ liệu mới từ ứng dụng WYN (hoặc bất kỳ nguồn nào khác như VietMap, OSM). Tuân thủ quy trình này sẽ đảm bảo hệ thống VietHUD hoạt động ổn định 100%, không bị mất đèn đỏ, không bị sai camera phạt nguội và không bị vỡ bản đồ.

---

## I. NGUYÊN TẮC CỐT LÕI (BẤT BIẾN)

### 1. Phân định dữ liệu lớp Vector đường và Dữ liệu Cảnh báo
* **Lớp Vector đường (Road Network):**
  * **LUÔN DÙNG DỮ LIỆU TỪ OPENSTREETMAP (OSM):** File `tiles.bin` (~744 MB) và `seg_names.bin` (~106 MB).
  * **Lý do:** OSM có đầy đủ 6 cấp độ đường từ cao tốc đến ngõ ngách, tên đường chuẩn và cấu trúc tile phân cấp tối ưu cho chip nhúng.
  * **CẢNH BÁO:** **KHÔNG BAO GIỜ** ghi đè `tiles.bin` bằng dữ liệu trích xuất từ Segment 2 của WYN `data.wmap`. Dữ liệu WYN chỉ dùng để lấy thuộc tính giới hạn tốc độ và cờ đường một chiều/hầm.

* **Lớp Dữ liệu Cảnh báo (Traffic Alerts):**
  * **`cameras.bin`:** Chứa vị trí camera phạt nguội (vượt đèn đỏ, đi sai làn) và camera đo tốc độ.
  * **`signs.bin`:** Chứa biển báo giới hạn tốc độ, khu dân cư, cấm vượt, hầm đường bộ VÀ toàn bộ đèn tín hiệu giao thông (Traffic Lights).

---

## II. ĐỊNH DẠNG CẤU TRÚC NHỊ PHÂN (BINARY STRUCT SPECIFICATION)

Rất nhiều lỗi trước đây xuất phát từ việc nhầm lẫn định dạng struct nhị phân. Luôn đối chiếu đúng bảng sau:

### 1. File `cameras.bin` (20 bytes / camera)
* **Python Struct:** `<QiihH` (Little-endian)
* **C++ Struct:**
  ```cpp
  struct CameraRecord {
      uint64_t osm_id;        // 8 bytes: ID định danh
      int32_t  lat_e7;        // 4 bytes: Vĩ độ * 1e7
      int32_t  lon_e7;        // 4 bytes: Kinh độ * 1e7
      int16_t  speed_kmh;     // 2 bytes: Tốc độ giới hạn (0 = Camera phạt nguội/đèn đỏ; >0 = Camera bắn tốc độ)
      uint16_t direction_deg; // 2 bytes: Góc hướng xe chạy đến (0-359 độ; 0xFFFF = Đa hướng 360 độ)
  }; // Tổng cộng: 20 bytes
  ```
* ⚠️ **LỖI KINH NGHIỆM:** Tuyệt đối không viết thành `<QiiHh`. Nếu đảo `speed` và `direction`, góc hướng 324° sẽ bị firmware đọc thành tốc độ 324 km/h, làm biến mất toàn bộ camera phạt nguội (`speed = 0`)!

### 2. File `signs.bin` (20 bytes / biển báo & đèn)
* **Python Struct:** `<QiiHH` (Little-endian)
* **C++ Struct:**
  ```cpp
  struct SignRecord {
      uint64_t osm_id;        // 8 bytes: ID định danh
      int32_t  lat_e7;        // 4 bytes: Vĩ độ * 1e7
      int32_t  lon_e7;        // 4 bytes: Kinh độ * 1e7
      uint16_t sign_type;     // 2 bytes: Mã loại biển báo
      uint16_t direction_deg; // 2 bytes: Góc hướng áp dụng (0-359 độ; 0xFFFF = Đa hướng 360 độ)
  }; // Tổng cộng: 20 bytes
  ```
* **Bảng mã `sign_type`:**
  * `1`: Giới hạn tốc độ (Speed limit)
  * `2`: Hết giới hạn tốc độ
  * `3`: Bắt đầu khu đông dân cư
  * `4`: Hết khu đông dân cư
  * `5`: Cấm vượt
  * `6`: Hết cấm vượt
  * `7`: **Đèn tín hiệu giao thông (Traffic Light)**
  * `8`: Hầm đường bộ (Tunnel)
  * `9`: Trạm thu phí (Toll booth)
  * `10`: Đoạn đường nguy hiểm

---

## III. QUY TẮC ĐÈN GIAO THÔNG TẠI NÚT GIAO (NGÃ 3, NGÃ 4)

Trước đây Hà Nội không có đèn đỏ nào vì WYN chỉ lưu biển báo quốc lộ trong Segment 3, còn đèn nội thành bị phân tán. Quy tắc xử lý chuẩn:

1. **Tổng hợp đa nguồn:**
   * Nguồn 1: OpenStreetMap (`highway=traffic_signals`) — toàn quốc có gần 8,000 điểm.
   * Nguồn 2: Dữ liệu trích xuất từ camera/đèn của WYN bản mới.
2. **Quy tắc bao phủ mọi hướng tiếp cận (All Approaches Rule):**
   * **Đèn trung tâm ngã tư (Junction Center):** BẮT BUỘC gán `direction_deg = 0xFFFF` (Đa hướng). Dù xe rẽ trái, rẽ phải, đi thẳng hay quay đầu từ bất cứ ngõ nào vào nút giao đều được cảnh báo.
   * **Đèn nhánh đón đầu (Approach Branches):** Tạo thêm các điểm đèn phụ đặt lùi 10 - 15m dọc theo các nhánh đường đi vào ngã tư với góc hướng đón chuẩn, giúp thuật toán lookahead của firmware tính chính xác khoảng cách giảm tốc.
3. **Khử trùng lặp (Spatial Deduplication):**
   * Bán kính gộp cụm là 15 - 20 mét để tránh tình trạng hiển thị nhiều icon đèn đè lên nhau tại cùng một cột đèn.

---

## IV. CÁC BƯỚC THỰC HIỆN KHI CÓ DỮ LIỆU WYN MỚI

Khi bạn có bản cập nhật mới từ WYN (ví dụ file `data.wmap` mới hoặc APK mới):

### Bước 1: Chuẩn bị dữ liệu đầu vào
1. Đặt file `data.wmap` mới vào `tools/map_builder/data.wmap`.
2. Đặt file danh sách camera mới (nếu có) vào `tools/map_builder/cameras.csv`.

### Bước 2: Chạy script tổng hợp tự động
Chạy script:
```bash
python tools/map_builder/complete_junction_lights.py
```
Script này sẽ:
* Đọc biển báo từ WYN mới.
* Tích hợp toàn bộ đèn tín hiệu từ OSM + WYN.
* Áp dụng quy tắc ngã 3, ngã 4 đa hướng (`0xFFFF`) và tạo đèn nhánh tiếp cận.
* Đóng gói đúng định dạng nhị phân chuẩn vào `speedmap/signs.bin` và `speedmap/cameras.bin`.

### Bước 3: Tạo Manifest và Ký số ECDSA (BẮT BUỘC)
Firmware VietHUD sẽ kiểm tra chữ ký mật mã trước khi đọc thẻ nhớ. Chạy:
```bash
python tools/sign_manifest.py speedmap/manifest.txt
```
* Kiểm tra `speedmap/manifest.txt`: chứa phiên bản mới, dung lượng byte và SHA256 chính xác.
* Kiểm tra file chữ ký `speedmap/manifest.txt.sig` đã được tạo mới thành công.

### Bước 4: Kiểm tra trực quan bằng Speedmap Studio
**TUYỆT ĐỐI KHÔNG COPY VÀO THẺ KHI CHƯA XEM THỬ QUA STUDIO:**
1. Chạy file `Chay_SpeedMap_Studio.bat`.
2. Mở trình duyệt tại `http://localhost:8088`.
3. Kiểm tra các tiêu chí:
   * **Đèn giao thông:** Phóng to vào khu vực Hà Nội (Ngã Tư Sở, Cầu Giấy, Hoàn Kiếm) xem các ngã tư có icon đèn giao thông màu xanh/đỏ hay không.
   * **Camera phạt nguội:** Xem các camera nội thành có hiển thị icon camera đỏ (`red_light_cam`) với tốc độ = 0 không.
   * **Đường vector:** Kiểm tra đường phố có hiển thị đầy đủ, không bị rỗng mạng lưới đường.

### Bước 5: Đồng bộ vào Thẻ nhớ SD
1. Cắm thẻ nhớ SD vào máy tính (ổ đĩa `E:\speedmap\`).
2. Copy các file đã cập nhật từ thư mục `speedmap/` vào `E:\speedmap\`:
   * `cameras.bin`
   * `signs.bin`
   * `manifest.txt`
   * `manifest.txt.sig`
   * *(Nếu có cập nhật tên đường hoặc index thì copy thêm `names.bin`, `index.bin`)*
3. Kiểm tra thư mục âm thanh `E:\speedmap\sounds\` có đủ các file âm thanh giọng nói tiếng Việt từ `sounds/vi/`.

### Bước 6: Đồng bộ lên GitHub
1. Thêm các file thay đổi vào Git:
   ```bash
   git add speedmap/ docs/ tools/
   git commit -m "feat(speedmap): update dataset"
   ```
2. Cập nhật tag phiên bản:
   ```bash
   git tag -f v3 -m "VietHUD V3 updated"
   ```
3. Đẩy lên GitHub:
   ```bash
   git push origin main --tags -f
   ```

---

## V. CHECKLIST KIỂM TRA NHANH (QUICK CHECKLIST)

| Hạng mục | Tiêu chuẩn đạt | Cách kiểm tra |
| :--- | :--- | :--- |
| **Vector Layer** | `tiles.bin` ~744 MB (OSM 6 cấp đường) | Kiểm tra dung lượng file trong `speedmap/` |
| **Cameras Format** | Struct `<QiihH` (20 bytes/cam) | Camera phạt nguội có `speed=0`, góc `H` |
| **Signs Format** | Struct `<QiiHH` (20 bytes/sign) | Đèn đỏ là `sign_type=7`, góc `H` |
| **Đèn ngã 3, ngã 4** | Tâm ngã 4 mang góc `0xFFFF` + có đèn nhánh | Mở Speedmap Studio tại Hà Nội thấy đầy đủ |
| **Ký số Manifest** | Có `manifest.txt.sig` hợp lệ | File chữ ký nhị phân ECDSA P-256 |
| **Kiểm thử Studio** | Xem trực quan tại `localhost:8088` | Icon đèn, camera hiển thị đúng vị trí |
| **Thẻ nhớ & GitHub** | Đã copy vào thẻ và push tag lên repo | Thẻ cắm vào VietHUD khởi động nhận ngay |