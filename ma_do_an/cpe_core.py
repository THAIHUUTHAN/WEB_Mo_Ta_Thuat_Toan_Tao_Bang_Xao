#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cpe_core.py — NHÂN THAM CHIẾU CHÍNH TẮC của hệ CPE (Encryption-then-Compression)
Đồ án: Thái Hữu Thân — AT19A — AT190149 — GVHD: ThS. Hoàng Thu Phương

ĐÂY LÀ FILE GỐC DUY NHẤT. Mọi script thí nghiệm (TN0..TN9) phải import từ đây,
không được cài đặt lại thuật toán. Nhân này đã bị dựng lại NĂM LẦN trong quá trình
chuẩn bị; lần này mọi quy ước đều được viết thành hằng số và chú thích có thể kiểm
chứng bằng cpe_vantay.py.

BẢY QUY ƯỚC BẮT BUỘC (thiếu bất kỳ cái nào là bên nhận KHÔNG giải mã được):
  Q1. Bản tin dẫn xuất: M = be32(counter_n) || be32(r)          — 8 byte
  Q2. Thẻ:              T = HMAC-SHA256(K_master, M)            — 32 byte
  Q3. Hạt giống:        u23 = (T[0]<<16 | T[1]<<8 | T[2]) & 0x7FFFFF
                        x0  = float32((u23 + 0.5) / 8388608.0)
  Q4. Bước lặp float32: x <- (r*x)*(1-x)  TÍNH TỪ TRÁI SANG PHẢI, r = 3.99f
      *** KHÔNG được viết r*(x*(1-x)) — cho quỹ đạo KHÁC HẲN, xem §so sánh dưới.
      *** KHÔNG được để trình biên dịch hợp nhất FMA: bắt buộc -ffp-contract=off
  Q5. WARMUP = 150 bước, bỏ đi.
  Q6. PP-A Fisher-Yates giảm dần: với i = N-1..1
        y = (2/pi) * asin(sqrt(x))   <-- tính từ x HIỆN TẠI
        j = floor(y * (i+1))         <-- ép về [0..i]
        swap(p[i], p[j])
        x = step(x)                  <-- LẶP X SAU KHI ĐÃ DÙNG
  Q7. PP-C xoshiro128**: s[k] = uint32 BIG-ENDIAN từ T[4k..4k+3], k = 0..3
        j = (uint32)((uint64)next() * (i+1) >> 32)   <-- LEMIRE, không chia dư
"""
import hmac, hashlib, struct, math, array
import numpy as np

# ------------------------- Tham số hệ thống đã chốt -------------------------
W, H          = 320, 240          # QVGA xám
BS            = 8                 # kích thước khối
NBLK          = (W // BS) * (H // BS)   # 1200
FRAME_BYTES   = W * H             # 76800
R_LOGISTIC    = 3.99
WARMUP        = 150
SHORT_CYCLE_T = 1350              # ngưỡng chu kỳ ngắn -> retry-nonce
K_MASTER_TEST = bytes(range(32))  # KHÓA THÍ NGHIỆM: 00 01 02 ... 1f

_f32  = np.float32
_R    = _f32(R_LOGISTIC)
_ONE  = _f32(1.0)
_C2PI = _f32(2.0 / math.pi)

# ------------------------------- Q1..Q3 -------------------------------------
def hmac_tag(counter_n: int, r: int = 0, key: bytes = K_MASTER_TEST) -> bytes:
    """Q1 + Q2. M = be32(counter_n) || be32(r); T = HMAC-SHA256(K, M)."""
    return hmac.new(key, struct.pack('>II', counter_n & 0xFFFFFFFF, r & 0xFFFFFFFF),
                    hashlib.sha256).digest()

def u23_from_tag(T: bytes) -> int:
    """Q3a. Cắt 23 bit đầu của thẻ."""
    return ((T[0] << 16) | (T[1] << 8) | T[2]) & 0x7FFFFF

def seed_from_u23(u23: int) -> np.float32:
    """Q3b. x0 = float32((u23+0.5)/2^23). Không bao giờ ra 0 hay 1."""
    return _f32((u23 + 0.5) / 8388608.0)

# --------------------------------- Q4 ---------------------------------------
def step(x: np.float32) -> np.float32:
    """Q4. x <- (r*x)*(1-x), float32, kết hợp TỪ TRÁI SANG PHẢI."""
    return _f32(_f32(_R * x) * _f32(_ONE - x))

def step_wrong_assoc(x: np.float32) -> np.float32:
    """Biến thể SAI r*(x*(1-x)) — chỉ dùng để đo mức lệch, KHÔNG dùng trong hệ."""
    return _f32(_R * _f32(x * _f32(_ONE - x)))

def cycle_info(u23: int, max_steps: int = 200000):
    """Floyd/brent đơn giản bằng bảng băm: trả (tail_len, cycle_len)."""
    x = seed_from_u23(u23)
    seen = {}
    i = 0
    while i < max_steps:
        b = int(x.view(np.uint32))
        if b in seen:
            return seen[b], i - seen[b]
        seen[b] = i
        x = step(x); i += 1
    return -1, -1

def is_short_cycle(u23: int) -> bool:
    t, c = cycle_info(u23)
    return c < 0 or (t + c) < SHORT_CYCLE_T

# --------------------------------- Q6 ---------------------------------------
def perm_A_from_u23(u23: int, n: int = NBLK) -> list:
    """PP-A: Logistic Map float32 + Fisher-Yates giảm dần."""
    x = seed_from_u23(u23)
    for _ in range(WARMUP):
        x = step(x)
    p = list(range(n))
    for i in range(n - 1, 0, -1):
        y = _f32(_C2PI * _f32(math.asin(math.sqrt(float(x)))))
        j = int(_f32(y * _f32(i + 1)))
        if j > i: j = i
        p[i], p[j] = p[j], p[i]
        x = step(x)
    return p

def perm_A(counter_n: int, r: int = 0, key: bytes = K_MASTER_TEST, n: int = NBLK) -> list:
    return perm_A_from_u23(u23_from_tag(hmac_tag(counter_n, r, key)), n)

# --------------------------------- Q7 ---------------------------------------
class Xoshiro128SS:
    """xoshiro128** — chỉ dùng số nguyên 32 bit, KHÔNG có rủi ro lệch nền tảng."""
    __slots__ = ('s',)
    def __init__(self, s):
        self.s = [v & 0xFFFFFFFF for v in s]
    @staticmethod
    def _rotl(x, k):
        return ((x << k) | (x >> (32 - k))) & 0xFFFFFFFF
    def next(self):
        s = self.s
        out = (self._rotl((s[1] * 5) & 0xFFFFFFFF, 7) * 9) & 0xFFFFFFFF
        t = (s[1] << 9) & 0xFFFFFFFF
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t
        s[3] = self._rotl(s[3], 11)
        return out

def seed_C_from_tag(T: bytes) -> list:
    """Q7a. 128 bit đầu của thẻ, mỗi 4 byte đọc BIG-ENDIAN."""
    return [struct.unpack('>I', T[4 * k:4 * k + 4])[0] for k in range(4)]

def perm_C_from_tag(T: bytes, n: int = NBLK) -> list:
    g = Xoshiro128SS(seed_C_from_tag(T))
    p = list(range(n))
    for i in range(n - 1, 0, -1):
        j = (g.next() * (i + 1)) >> 32          # Q7b: Lemire
        p[i], p[j] = p[j], p[i]
    return p

def perm_C(counter_n: int, r: int = 0, key: bytes = K_MASTER_TEST, n: int = NBLK) -> list:
    return perm_C_from_tag(hmac_tag(counter_n, r, key), n)

# ------------------------- Áp hoán vị lên framebuffer ------------------------
def permute_blocks(buf: bytes, perm: list, w: int = W, h: int = H, bs: int = BS) -> bytearray:
    """Không tại chỗ: out[khối i] = in[khối perm[i]]. Tốn thêm 76800 B RAM."""
    bw = w // bs
    out = bytearray(len(buf))
    for i, src in enumerate(perm):
        di, dj = divmod(i, bw); si, sj = divmod(src, bw)
        for row in range(bs):
            d = (di * bs + row) * w + dj * bs
            s = (si * bs + row) * w + sj * bs
            out[d:d + bs] = buf[s:s + bs]
    return out

def permute_blocks_inplace(buf: bytearray, perm: list, w: int = W, h: int = H, bs: int = BS):
    """Tại chỗ theo CHU TRÌNH. Tiết kiệm 76800 B, chỉ tốn 1 khối tạm (64 B) + cờ."""
    bw = w // bs
    nb = len(perm)
    def rd(idx):
        bi, bj = divmod(idx, bw)
        return bytes(b for row in range(bs)
                     for b in buf[(bi * bs + row) * w + bj * bs:(bi * bs + row) * w + bj * bs + bs])
    def wr(idx, blk):
        bi, bj = divmod(idx, bw)
        for row in range(bs):
            o = (bi * bs + row) * w + bj * bs
            buf[o:o + bs] = blk[row * bs:(row + 1) * bs]
    done = bytearray(nb)
    for start in range(nb):
        if done[start] or perm[start] == start:
            done[start] = 1; continue
        tmp = rd(start); cur = start
        while True:
            src = perm[cur]
            done[cur] = 1
            if src == start:
                wr(cur, tmp); break
            wr(cur, rd(src)); cur = src
    return buf

def inverse_perm(perm: list) -> list:
    inv = [0] * len(perm)
    for i, v in enumerate(perm): inv[v] = i
    return inv

# ------------------------------ Bản tin MQTT --------------------------------
def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def build_header(flags: int, r: int, counter_n: int, length: int, crc: int) -> bytes:
    """12 byte big-endian: MAGIC 0x43 | VER 0x01 | FLAGS | r | counter(4) | LEN(2) | CRC(2)"""
    return struct.pack('>BBBBIHH', 0x43, 0x01, flags & 0xFF, r & 0xFF,
                       counter_n & 0xFFFFFFFF, length & 0xFFFF, crc & 0xFFFF)

# --------------------------------- Vân tay -----------------------------------
def fnv1a64(b: bytes) -> int:
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def perm_fnv(p: list) -> int:
    """QUY ƯỚC VÂN TAY: mã hóa perm thành uint16 LITTLE-ENDIAN rồi FNV-1a 64 bit."""
    return fnv1a64(array.array('H', p).tobytes())

def xorshift32_image(seed=0x12345678, nbytes=FRAME_BYTES) -> bytes:
    """Ảnh mẫu tất định dùng chung cho mọi phép đối chiếu PC <-> ESP32."""
    x = seed & 0xFFFFFFFF
    out = bytearray(nbytes)
    for i in range(nbytes):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = (x >> 24) & 0xFF
    return bytes(out)
