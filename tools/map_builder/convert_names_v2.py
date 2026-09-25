#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
   VIETHUD NAMES & SEG_NAMES V2 UPGRADER & VALIDATOR
   Chuyển đổi và nâng cấp names.bin và seg_names.bin lên chuẩn V2 (32-bit ID)
================================================================================
Đặc tả định dạng V2 (Chuẩn firmware ESP32-S3):
  1. names.bin:
     - magic[4]       : "VNNM" (4 bytes)
     - version        : 2 (uint16, 2 bytes)
     - count          : Số lượng tên đường (uint32, 4 bytes)
     - poolBytes      : Kích thước chuỗi UTF-8 string pool (uint32, 4 bytes)
     - reserved       : 0 (uint16, 2 bytes)
     ==> Tổng Header  : đúng 16 bytes (<4sHIIH)
     - offsets[count] : Mảng offset vị trí từng tên (uint32 each, count * 4 bytes)
     - pool[poolBytes]: Chuỗi ký tự UTF-8, mỗi tên kết thúc bằng byte NUL (0x00)

  2. seg_names.bin:
     - Mảng phẳng uint32 (<I, 4 bytes mỗi segment), index = segId (1-based, index 0 bỏ trống).
     - Giá trị = nameId tương ứng trong names.bin (0 = không có tên đường).
"""

import os
import sys
import struct
import argparse

VNNM_MAGIC = b"VNNM"

def upgrade_names_file_to_v2(names_path):
    """Nâng cấp file names.bin từ V1 lên V2 (hoặc xác thực nếu đã là V2)."""
    if not os.path.exists(names_path):
        print(f"[!] File names.bin khong ton tai: {names_path}")
        return False, 0

    with open(names_path, "rb") as f:
        data = f.read()

    if len(data) < 16:
        print(f"[!] File names.bin qua ngan ({len(data)} bytes): {names_path}")
        return False, 0

    magic = data[:4]
    if magic != VNNM_MAGIC:
        print(f"[!] File names.bin sai magic ({magic}): {names_path}")
        return False, 0

    version = struct.unpack("<H", data[4:6])[0]
    if version == 2:
        # Đã là V2, kiểm tra cấu trúc
        count, pool_bytes, res = struct.unpack("<IIH", data[6:16])
        expected_len = 16 + (count * 4) + pool_bytes
        print(f"[*] names.bin da la V2: count={count:,}, pool_bytes={pool_bytes:,} B (File size: {len(data):,} B, expected {expected_len:,} B)")
        return True, count

    if version == 1:
        # V1 Header: magic[4], ver(u16)=1, count(u16), pool_bytes(u32), reserved(u32)
        count_v1, pool_bytes, res32 = struct.unpack("<HII", data[6:16])
        offsets_and_pool = data[16:]
        expected_v1 = 16 + (count_v1 * 4) + pool_bytes
        if len(data) != expected_v1:
            print(f"[!] Canh bao: Kich thuoc V1 thuc te ({len(data)}) != tinh toan ({expected_v1})")

        # Tạo V2 Header: magic[4]="VNNM", version(u16)=2, count(u32), pool_bytes(u32), reserved(u16)=0
        v2_header = struct.pack("<4sHIIH", VNNM_MAGIC, 2, count_v1, pool_bytes, 0)
        v2_data = v2_header + offsets_and_pool

        # Sao lưu file cũ
        bak_path = names_path + ".v1.bak"
        if not os.path.exists(bak_path):
            with open(bak_path, "wb") as f:
                f.write(data)

        with open(names_path, "wb") as f:
            f.write(v2_data)

        print(f"[OK] Da nang cap names.bin len V2: {names_path}")
        print(f"     + Version: 1 -> 2")
        print(f"     + Count  : {count_v1:,} (u32)")
        print(f"     + Pool   : {pool_bytes:,} bytes (u32)")
        return True, count_v1

    print(f"[!] Phien ban names.bin khong ho tro (v{version}): {names_path}")
    return False, 0

def is_seg_names_already_v2(data, name_count=0):
    """Kiem tra xem buffer seg_names.bin da o dinh dang uint32 (V2) hay chua."""
    file_size = len(data)
    if file_size % 4 != 0 or file_size < 16:
        return False
    check_cnt = min(file_size // 4, 100)
    entries_u32 = struct.unpack(f"<{check_cnt}I", data[:check_cnt * 4])
    entries_u16 = struct.unpack(f"<{check_cnt * 2}H", data[:check_cnt * 4])
    odd_zeros = sum(1 for i in range(1, len(entries_u16), 2) if entries_u16[i] == 0)
    even_nonzeros = sum(1 for i in range(0, len(entries_u16), 2) if entries_u16[i] != 0)
    if odd_zeros >= (len(entries_u16) // 2) - 2 and (even_nonzeros > 0 or check_cnt < 5):
        if name_count > 0:
            return all(val <= name_count for val in entries_u32)
        return True
    return False

def upgrade_seg_names_to_v2(seg_names_path, name_count=0):
    """Nâng cấp seg_names.bin từ mảng uint16 lên uint32 (mỗi segment 4 bytes)."""
    if not os.path.exists(seg_names_path):
        print(f"[!] File seg_names.bin khong ton tai: {seg_names_path}")
        return False

    with open(seg_names_path, "rb") as f:
        data = f.read()

    file_size = len(data)
    if file_size == 0:
        return False

    if is_seg_names_already_v2(data, name_count):
        print(f"[*] seg_names.bin da o chuan V2 (uint32 entries, size: {file_size:,} B, {file_size//4:,} segments)")
        return True

    # Giả định file hiện tại là V1 (uint16 mỗi entry)
    num_segs = file_size // 2
    print(f"[*] Dang nang cap seg_names.bin tu uint16 sang uint32 ({num_segs:,} segments)...")
    
    u16_entries = struct.unpack(f"<{num_segs}H", data[:num_segs * 2])
    v2_data = struct.pack(f"<{num_segs}I", *u16_entries)

    bak_path = seg_names_path + ".v1.bak"
    if not os.path.exists(bak_path):
        with open(bak_path, "wb") as f:
            f.write(data)

    with open(seg_names_path, "wb") as f:
        f.write(v2_data)

    print(f"[OK] Da nang cap seg_names.bin len V2 thanh cong: {seg_names_path}")
    print(f"     + Kich thuoc cu (u16): {file_size:,} bytes")
    print(f"     + Kich thuoc moi (u32): {len(v2_data):,} bytes ({num_segs:,} segments)")
    return True

def upgrade_speedmap_directory(speedmap_dir):
    """Nâng cấp cả names.bin và seg_names.bin trong thư mục speedmap."""
    print("=" * 80)
    print(f"KIEM TRA VA NANG CAP NAMES V2 CHO: {speedmap_dir}")
    print("=" * 80)
    names_f = os.path.join(speedmap_dir, "names.bin")
    seg_names_f = os.path.join(speedmap_dir, "seg_names.bin")

    ok1, count = upgrade_names_file_to_v2(names_f)
    ok2 = upgrade_seg_names_to_v2(seg_names_f, name_count=count)
    return ok1 and ok2

def main():
    parser = argparse.ArgumentParser(description="VietHUD names.bin and seg_names.bin V2 Upgrader")
    parser.add_argument("path", nargs="?", default=None, help="Duong dan den thu muc speedmap hoac file names.bin")
    args = parser.parse_args()

    target_dirs = []
    if args.path:
        if os.path.isdir(args.path):
            target_dirs.append(args.path)
        elif os.path.isfile(args.path):
            target_dirs.append(os.path.dirname(args.path))
    else:
        # Tự động quét các thư mục speedmap trong dự án
        script_dir = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.path.join(r"C:\Users\phamq\Downloads\Data\VietHUD_SDCard_Ready", "speedmap"),
            os.path.join(r"C:\Users\phamq\radar_car\speedmap"),
            os.path.join(r"C:\Users\phamq\radar_car\data\speedmap"),
            os.path.join(r"C:\Users\phamq\Downloads\Data\VietHUD_Git_Staging", "speedmap"),
            os.path.join(r"C:\Users\phamq\Downloads\Data", "speedmap"),
        ]
        for c in candidates:
            if os.path.exists(c) and os.path.exists(os.path.join(c, "names.bin")):
                target_dirs.append(c)

    if not target_dirs:
        print("[!] Khong tim thay thu muc speedmap nao de nang cap!")
        sys.exit(1)

    for td in target_dirs:
        upgrade_speedmap_directory(td)
        print()

    print("=" * 80)
    print("HOAN TAT KIEM TRA VA NANG CAP TOAN BO DU LIEU NAMES LEN V2!")
    print("=" * 80)

if __name__ == "__main__":
    main()
