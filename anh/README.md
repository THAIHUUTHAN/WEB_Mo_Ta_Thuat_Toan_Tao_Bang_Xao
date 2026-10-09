# Thư mục ảnh

Đặt ảnh vào đây (jpg, jpeg, png, bmp, tif, tiff, webp, gif, pgm) rồi chạy
`python tools/dung_trang.py`.

- Ảnh đúng 320 × 240 xám được giữ nguyên từng điểm ảnh. Đây là cách sát đồ án nhất:
  dùng lại 10 ảnh thử đã hạ về 320 × 240 xám của đồ án, hoặc ảnh do chính ESP32-CAM chụp.
- Ảnh khác kích thước được đổi sang xám rồi cắt giữa và thu về 320 × 240 (Pillow, LANCZOS).
- Thứ tự xử lý theo tên tệp; ảnh thứ i nhận số khung n = n0 + i (mặc định n0 = 0).
- `mau_esp32cam.png` là ảnh mẫu tổng hợp, có thể xóa.
