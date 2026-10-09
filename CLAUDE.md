# CLAUDE.md — kho doan-cpe-web

## Kho này là gì

Mô hình web của đồ án tốt nghiệp "Nghiên cứu phương pháp mã hóa cảm nhận và ứng dụng
trong IoT" (Thái Hữu Thân, AT19A, AT190149; GVHD ThS. Hoàng Thu Phương). Hệ thống: ESP32-CAM
chụp ảnh xám 320×240, xáo 1 200 khối 8×8 bằng bộ sinh PP-C (HMAC-SHA256 → xoshiro128** →
Fisher–Yates + Lemire), nén JPEG Q = 20, gửi qua MQTT; máy chủ sinh lại cách xáo từ số khung
và khôi phục ảnh.

| Đường dẫn | Vai trò |
|---|---|
| `index.html` | Trang web giải thích khóa bí mật + số khung n tạo ra cách xáo: trình chiếu 7 bước, vì sao 2^128, mã nguồn có chú thích. Ảnh minh họa `anh/nt01_khu_vuon.png` |
| `anh/` | Người dùng đặt ảnh vào đây (jpg, png, bmp, tif, webp) |
| `ban_tin_esp32/` | Tùy chọn: bản tin `.bin` bắt thật từ MQTT của ESP32-CAM |
| `ma_do_an/` | Mã nguồn của đồ án, sao nguyên văn. **Không sửa.** |
| `tools/dung_trang.py` | Chạy chuỗi xử lý trên `anh/` và `ban_tin_esp32/`, sinh `data/` |
| `tools/bat_ban_tin.py` | Chạy trên máy có broker để lưu bản tin thật thành `.bin` |
| `data/` | Sinh tự động. Không sửa tay. |

## Lệnh

```bash
pip install -r requirements.txt
python tools/dung_trang.py        # chạy lại sau MỌI thay đổi ảnh, mã hoặc trang
python -m http.server 8000        # xem thử tại http://localhost:8000
```

## Quy tắc bất biến

1. Không sửa thuật toán trong `ma_do_an/`. Không cài đặt lại HMAC, xoshiro128**, Lemire,
   Fisher–Yates, tiêu đề 12 byte hay CRC bằng Python ở chỗ khác: `tools/` chỉ được gọi hàm
   trong `ma_do_an/cpe_core.py` và `ma_do_an/tn16_receiver.py`.
2. Phần lõi JavaScript trong `index.html` (`sha256`, `hmacTag`, `seedFromTag`, `xoshiroNext`,
   `permFromState`, `permFnv`) chỉ để minh họa và không được đổi thuật toán. Sau mọi chỉnh
   sửa trang, mở trang và xác nhận vân tay chạy ngầm ghi "Khớp 4/4" (console, hoặc thuộc tính
   `data-van-tay` của thẻ `<html>`); nếu lệch, trang hiện dải cảnh báo đỏ.
3. `tools/dung_trang.py` tự dừng nếu vân tay Bảng 3.5 lệch hoặc tự kiểm bên nhận TN16 hỏng,
   và báo lỗi nếu có ảnh không khôi phục trùng từng byte. Không nới, không bỏ các phép kiểm này.
4. Số liệu phải nói rõ đo ở đâu. Số do `tools/` sinh ra là số đo trên **máy tính**
   (Pillow/libjpeg-turbo), không phải trên ESP32. Không trích lẫn số đo máy tính và số đo
   thiết bị. Không viết con số nào chưa đo; cái gì chưa đo thì ghi "chưa đo".
5. Chỉ dùng khóa thử `00 01 … 1f` (`K_MASTER_TEST`). Không bao giờ đưa khóa thật vào kho.
6. Trang viết bằng tiếng Việt, câu ngắn, dễ hiểu cho người không học mật mã. Giữ phong cách
   sẵn có (biến CSS ở `:root`, có chế độ tối, dùng được trên điện thoại).
7. Số bảng (3.3, 3.5, 4.12, …) theo bản thảo V2.1. Nếu bản cuối của đồ án được đưa vào kho
   thì đối chiếu và sửa lại số bảng.
8. Trang chỉ giải thích quá trình, không dẫn số liệu đo hay số bảng của đồ án. Mã nguồn trên trang
   lấy từ Phụ lục C bản V2.10 (C.1–C.3): giữ nguyên từng dòng lệnh, chỉ viết lại chú thích.

## Khi xong việc

Tóm tắt ngắn: đã đổi gì; kết quả vân tay; số ảnh xử lý; số ảnh khôi phục trùng từng byte;
phình kích thước trung vị và dải. Commit cả `data/`.
