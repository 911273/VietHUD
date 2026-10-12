# Vỏ hộp VietHUD Lite

Mô hình có tham số: `viethud_lite_case.scad` (OpenSCAD 2021+). Gồm màn hình + mạch Xingzhi Cube 1.54", loa chữ nhật 25×25×8, module GPS 15×15×10 (dây JST SH 1.0 mm 4 chân), **không pin** (cấp nguồn USB-C).

Bố trí, **nhìn thẳng vào màn hình**:
- **Mặt trên:** 3 nút chức năng ở phía trước, ngăn GPS ở phía sau (anten quay lên trời).
- **Bên trái:** cổng USB-C. **Bên phải:** nút nguồn.
- **Mặt trước:** màn hình có mái che nắng, lỗ micro ở góc dưới bên trái.
- **Nắp sau:** loa và lưới thoát âm.

| File | Nội dung | In |
|---|---|---|
| `case_front.stl` | Vỏ chính: viền màn hình, mái che, ngăn GPS | Mặt trước úp xuống bàn, cần support cho mái che và ngăn GPS |
| `case_cover.stl` | Nắp sau: khung loa, lưới loa, 4 cột ép mạch, nắp ngăn GPS | Mặt ngoài úp xuống, không cần support |
| `case_plungers.stl` | 4 nút bấm (3 nút trên + nút nguồn) | Đứng, 100% infill |

Kích thước ngoài với số mặc định: **45 × 49 × 33 mm**. Ngăn GPS cao thêm 12 mm trên nóc, mái che nhô ra phía trước 12 mm.

Thân hộp sâu 33 mm vì ngăn GPS phải nằm sau 3 nút trên nóc. Phần không gian dư phía sau mạch giúp thoáng nhiệt và để gọn dây.

## Vật liệu
**PETG hoặc ASA**, không dùng PLA: taplo ô tô đỗ nắng vượt 70°C, PLA mềm ra từ khoảng 55°C. Nên in màu sáng để đỡ hấp nhiệt. Nên dùng infill 30%, 3 lớp tường.

## Cần đo trước khi in (sửa trong khối `MEASURE`)
Mọi tọa độ trái/phải tính khi **nhìn thẳng vào màn hình** (dương = sang phải / lên trên).
1. `pcb_w`, `pcb_h`: dài/rộng mạch. `disp_t`: độ dày màn hình tính từ kính tới mặt trước mạch. `comp_t`: linh kiện cao nhất ở mặt sau mạch.
2. `disp_out_w/h`: viền kính màn hình. `scr_w/h`: vùng hiển thị. `scr_dx/dy`: độ lệch tâm màn hình.
3. `btn_x`: vị trí 3 nút trên nóc so với tâm. `btn_z_off`: tâm nút cách mặt sau mạch bao nhiêu.
4. `usb_y`: độ cao cổng USB-C bên trái. `pwr_y`: độ cao nút nguồn bên phải. `usb_z_off` và `pwr_z_off`: khoảng cách tới mặt sau mạch.
5. `mic_pos`: lỗ micro. `post_inset`: 4 cột ép mạch phải tì vào chỗ trống, không đè lên linh kiện.

Xuất lại STL:
```
openscad -o case_front.stl    -D 'part="front"'    viethud_lite_case.scad
openscad -o case_cover.stl    -D 'part="cover"'    viethud_lite_case.scad
openscad -o case_plungers.stl -D 'part="plungers"' viethud_lite_case.scad
```
Xem trước: `part="assembly"` hoặc `part="exploded"`.

## Lắp ráp
1. Đặt nút bấm từ bên trong vỏ, gờ của nút quay vào trong.
2. Úp màn hình và mạch vào ô màn hình: cổng USB-C hướng sang trái, nút nguồn sang phải.
3. Nhét module GPS vào ngăn trên nóc từ phía sau, **mặt anten gốm quay lên trên**. Luồn đầu cắm JST qua khe 8×5 mm ở sàn ngăn xuống mạch.
4. Đặt loa vào khung trên nắp sau. Dán hoặc nhỏ keo nến để chống rung.
5. Đóng nắp sau. Bắt 2 vít **M2×6 tự ren** qua tường trên và tường dưới.
6. Gắn lên taplo bằng **băng keo 3M VHB** dưới đáy, hoặc đế dính kính lái.

## Ghi chú thiết kế
- **Ngăn GPS:** nắp phía trên dày 1,2 mm, không có kim loại phía trên anten, cách mạch ESP32 một lớp tường. Đặt hộp sao cho ngăn GPS nhìn thẳng lên trời, tránh vùng kính lái có lớp phủ kim loại hoặc dây sấy.
- **Tản nhiệt:** khe thoáng ở đáy (sau mạch) và trên nóc (giữa các nút).
