# doan-cpe-web — mô hình web của đồ án mã hóa cảm nhận trên ESP32-CAM

Đặt ảnh vào thư mục `anh/`, chạy một lệnh, và trang `index.html` hiển thị mỗi ảnh đi qua
đúng chuỗi xử lý của đồ án: xám 320×240 → xáo 1 200 khối bằng PP-C → JPEG Q = 20 → bản tin
12 byte tiêu đề + CRC → bên nhận kiểm CRC, giải nén, xáo ngược → ảnh khôi phục.

Mọi phép mật mã và giải mã gọi thẳng mã của đồ án trong `ma_do_an/`
(`cpe_core.py`, `tn16_receiver.py`). Trước khi xử lý ảnh, script kiểm 5 vân tay của
Bảng 3.5 và chạy tự kiểm của bên nhận TN16; lệch là dừng.

## Dùng trên máy

```bash
pip install -r requirements.txt
# chép ảnh vào anh/
python tools/dung_trang.py
python -m http.server 8000      # rồi mở http://localhost:8000
```

Mở thẳng `index.html` bằng trình duyệt cũng được. Cần mạng chỉ để tải phông chữ và tô
màu mã nguồn.

Tham số: `--q 10|20|30|40` (mặc định 20, đúng cấu hình đồ án), `--n0` số khung của ảnh đầu.

## Trang hiển thị gì

- **Ảnh chạy qua hệ thống**: số ảnh, số ảnh khôi phục trùng từng byte, phình kích thước
  trung vị và dải; với từng ảnh: ảnh gốc → ảnh mã trên đường truyền → ảnh khôi phục, 12 byte
  tiêu đề, 128 công tắc, kích thước JPEG có và không mã hóa, PSNR/SSIM của ảnh mã; tải được
  bản tin `.bin`.
- **Máy tạo cách xáo**: ví dụ 3 ô 2 công tắc, máy thật từng bước, vì sao có 2^128 cách
  xáo, tự kiểm vân tay, mã nguồn trích nguyên văn, bốn điều nên nói kèm.

## Sát đồ án tới đâu

| Phần | Giống đồ án | Khác đồ án |
|---|---|---|
| Khóa, HMAC, PP-C, hoán vị khối | Gọi đúng `cpe_core.py`, khóa thử 00…1f | — |
| Tiêu đề 12 byte, FLAGS, CRC | Giống `do_one_frame()` của firmware | — |
| Bên nhận | Gọi đúng `parse_frame`, `decode_frame` của TN16 | — |
| Nén JPEG | Q = 20, Pillow/libjpeg-turbo như các thí nghiệm trên máy tính | Không phải bộ nén `fmt2jpg` trên ESP32 |
| Ảnh vào | 320×240 xám giữ nguyên | Ảnh khác cỡ được thu về bằng Pillow |
| Thời gian, MQTT, năng lượng | — | Không mô phỏng; xem số đo thiết bị trong đồ án |

Muốn sát nhất: dùng 10 ảnh thử 320×240 xám của đồ án, và bản tin thật bắt từ ESP32-CAM
(`ban_tin_esp32/README.md`).

## Dùng với Claude Code trên web

1. Đưa toàn bộ thư mục này lên một kho GitHub (riêng tư cũng được).
2. Vào claude.ai/code, kết nối GitHub. Kho riêng tư cần cài Claude GitHub App cho kho đó.
3. Chọn kho, chọn chế độ, rồi giao việc. `CLAUDE.md` chứa các quy tắc Claude phải theo
   trong mọi phiên.
4. Claude đẩy kết quả lên một nhánh; xem diff rồi tạo PR và gộp vào `main`.

## Đăng lên mạng (tùy chọn)

`.github/workflows/trang.yml` tự chạy `tools/dung_trang.py` và đăng lên GitHub Pages mỗi lần
đẩy lên `main`. Bật ở Settings > Pages > Source = GitHub Actions. Trang Pages là công khai;
GitHub Pages cho kho riêng tư cần gói GitHub trả phí.
