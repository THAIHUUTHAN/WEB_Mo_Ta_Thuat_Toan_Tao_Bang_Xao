# Bản tin thật từ ESP32-CAM (tùy chọn)

Đặt ở đây các tệp `.bin`, mỗi tệp là nguyên một bản tin MQTT trên topic `cpe/<id>/frame`
(12 byte tiêu đề + JPEG). `tools/dung_trang.py` sẽ giải mã chúng bằng đúng `parse_frame`
và `decode_frame` của `tn16_receiver.py` rồi hiển thị trên trang như "bản tin thật".

Cách bắt bản tin: trên máy chạy broker mosquitto (như TN16), trong lúc ESP32-CAM đang gửi,
chạy thêm

    pip install paho-mqtt==2.1.0
    python tools/bat_ban_tin.py --host 127.0.0.1 --dev esp01 --so-khung 20

Firmware phải dùng khóa thử `00 01 … 1f` (đúng như tn17.ino) thì trang mới khôi phục được.
