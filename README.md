# Máy tạo cách xáo (PP-C) — trang minh họa cho báo cáo đồ án

Trang web một tệp giải thích cách khóa chủ biến thành cách xáo 1 200 ô ảnh
trong đồ án "Nghiên cứu phương pháp mã hóa cảm nhận và ứng dụng trong IoT".

## Có gì trong thư mục

- `index.html` — toàn bộ trang (HTML, CSS, JavaScript trong một tệp). Mở trực tiếp
  bằng trình duyệt là chạy. Cần mạng chỉ để tải phông chữ (Be Vietnam Pro,
  JetBrains Mono) và tô màu mã nguồn (highlight.js 11.9.0); không có mạng trang
  vẫn chạy đúng, chỉ đổi phông.
- `README.md` — tệp này.

## Nội dung trang

1. Ví dụ tí hon: 3 ô, 2 công tắc, bấm để thấy vì sao số cách xáo bằng số thế công tắc.
2. Máy thật từng bước với số thật (khóa thử 00 01 … 1f, khung 0): khóa chủ, số khung,
   HMAC-SHA256, 128 công tắc (bấm được), 3 lượt rút đầu, ảnh gốc và ảnh sau khi xáo.
3. Thanh trượt số công tắc → số thế (2^128 viết đủ 39 chữ số).
4. Tự kiểm vân tay Bảng 3.5: trang tính lại và so với giá trị kỳ vọng (hiện khớp 4/4).
5. Mã nguồn trích nguyên văn: `cpe_core.py`, firmware ESP32 (tn17.ino, vùng CORE
   giữ nguyên văn từ TN16), `tn16_receiver.py` (hàm decode_frame), cùng bản
   JavaScript mà trang đang chạy.
6. Bốn điều nên nói kèm về con số 2^128.

## Nguồn của các đoạn mã

Trích từ bản sao trong Project "Đồ án" trên claude.ai:

| Thẻ trên trang | Tệp gốc | Dòng |
|---|---|---|
| cpe_core.py | claude_cpe_core_nhan_tham_chieu_py.md (= cpe_core.py) | 1–51, 105–135, 137–148, 176–179, 195–204 |
| Firmware ESP32 (C) | claude_tn17_firmware_ino.md (= tn17.ino) | 74–77, 126–168, 181–195, 243–257, 557–571, 462–482 |
| Bên nhận | claude_tn16_receiver_py.md (= tn16_receiver.py) | 41–52 |

Việc nên làm khi máy Asus bật: so các đoạn trên với `C:\DoAn_CPE\code\cpe_core.py`
và firmware đang dùng thật. Nếu có khác biệt, thay đoạn mã trong `index.html`
và mở lại trang để chắc phần vân tay vẫn khớp 4/4.

## Đưa lên một phiên Claude Code trên web

Phiên cloud của Claude Code làm việc trên một kho GitHub, nên cách đơn giản nhất:

1. Tạo một kho GitHub (riêng tư cũng được), ví dụ `doan-cpe-web`.
2. Đưa `index.html` và `README.md` vào kho.
3. Mở phiên Claude Code trên kho đó và dán lời nhắn mẫu dưới đây nếu muốn
   Claude chỉnh tiếp.

Lời nhắn mẫu:

> Trong kho này có index.html, trang minh họa bộ sinh hoán vị PP-C của đồ án
> (HMAC-SHA256 → xoshiro128** → Fisher–Yates + Lemire, 1 200 khối 8×8).
> Đừng đổi thuật toán trong phần lõi JavaScript. Sau mọi chỉnh sửa, mở trang và
> xác nhận mục "Trang này chạy đúng thuật toán của đồ án" vẫn khớp 4/4 vân tay:
> T[0…7] n=0: 9f 0c d9 b9 40 97 fe 49; π[0…7] n=0: 109 1171 266 101 493 889 1106 419;
> FNV-1a 64 n=0: 0xE4D8CAD02C139271; n=12345: 0x77CDD08E5E99A22D.

Nếu chỉ cần trình chiếu, có thể bỏ qua bước trên: trang đã được đăng sẵn dưới dạng
artifact trên claude.ai và mở được bằng đường link.

## Lưu ý khi báo cáo

- Chú thích trong mã đánh số quy ước theo bản cũ (PP-C là Q7, Lemire là Q7b);
  Bảng 3.3 của đồ án đánh số lại Q1–Q7. Thuật toán giống hệt nhau.
- Số bảng (3.3, 3.5, 4.12) theo bản thảo V2.1; đối chiếu lại nếu bản cuối đánh số khác.
- Gạt tay đúng 1 công tắc ở bước 4 thì trung bình khoảng 4 ô giữ nguyên chỗ (không
  phải 1), vì vài lượt rút đầu của xoshiro128** gần như không đổi. Trong hệ thật điều
  này không xảy ra: giữa hai khung liền nhau HMAC đổi trung bình 64/128 công tắc và
  số ô trùng chỗ trung bình đúng bằng 1 (đã đo trên 300 cặp khung).
