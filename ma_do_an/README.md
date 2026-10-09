# Mã nguồn của đồ án — sao nguyên văn, không sửa

| Tệp | Vai trò trong đồ án | Nguồn bản sao |
|---|---|---|
| `cpe_core.py` | Nhân tham chiếu dùng chung (mục 3.4): HMAC, PP-A, PP-C, hoán vị khối, CRC, tiêu đề, vân tay | Project "Đồ án" trên claude.ai |
| `tn16_receiver.py` | Bên nhận chính thức TN16: kiểm CRC, sinh lại hoán vị, giải nén, hoán vị nghịch | Project "Đồ án" trên claude.ai |
| `tn17.ino` | Firmware ESP32-CAM; vùng CORE giữ nguyên văn từ TN16 | Project "Đồ án" trên claude.ai |

Đây là bản sao trong Project, có thể cũ hơn bản trên máy. Khi có máy, thay ba tệp bằng bản
trong `C:\DoAn_CPE\code\` và firmware đang dùng thật, rồi chạy `python tools/dung_trang.py`:
script sẽ dừng ngay nếu vân tay Bảng 3.5 hoặc tự kiểm bên nhận không còn đạt.

`tools/dung_trang.py` import trực tiếp `cpe_core` và `tn16_receiver` từ thư mục này.
`tn16_receiver.py` chỉ import `paho.mqtt` bên trong hàm chạy thật, nên import để dùng
`parse_frame`/`decode_frame` không cần paho.
