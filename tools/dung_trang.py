#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dung_trang.py — chạy ĐÚNG chuỗi xử lý của đồ án trên mọi ảnh trong anh/
và sinh dữ liệu cho mục "Ảnh chạy qua hệ thống" của index.html.

    python tools/dung_trang.py                 # Q = 20, khóa thử 00..1f, khung bắt đầu từ 0
    python tools/dung_trang.py --q 30 --n0 1000

Bên gửi (giống firmware tn17.ino, chế độ PP-C):
    ảnh -> xám 320x240 -> T = HMAC-SHA256(K, be32(n) || be32(r)) -> hoán vị PP-C
        -> xáo 1 200 khối 8x8 -> JPEG chất lượng Q -> tiêu đề 12 byte + CRC-16 -> BẢN TIN
Bên nhận (gọi thẳng parse_frame và decode_frame của tn16_receiver.py):
    BẢN TIN -> kiểm CRC -> giải nén JPEG -> sinh lại PP-C từ n -> xáo ngược -> ảnh khôi phục

Quy tắc: mọi phép mật mã, đóng gói và giải mã đều gọi mã của đồ án trong ma_do_an/.
Tệp này KHÔNG cài đặt lại thuật toán. Trước khi xử lý ảnh, nó kiểm vân tay
Bảng 3.5 và chạy tự kiểm của bên nhận; lệch bất kỳ điều gì là dừng ngay.

Mọi số đo ở đây là số đo trên MÁY TÍNH (bộ nén JPEG của Pillow/libjpeg-turbo),
không phải bộ nén JPEG trên ESP32. Không trích lẫn hai loại số đo.
"""
import argparse, contextlib, io, json, os, re, shutil, statistics, sys, time, unicodedata
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

GOC = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(GOC / "ma_do_an"))
import cpe_core as C          # noqa: E402  nhân tham chiếu của đồ án
import tn16_receiver as R     # noqa: E402  bên nhận chính thức của đồ án

Q_IDX = {10: 0, 20: 1, 30: 2, 40: 3}       # bảng chỉ số Q trong trường FLAGS (firmware: Q=20 -> 1)
PERM_MODE_PPC = 2
DUOI_ANH = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp", ".gif", ".pgm"}

THU_ANH = GOC / "anh"
THU_BANTIN_THAT = GOC / "ban_tin_esp32"
THU_DATA = GOC / "data"


# ----------------------------------------------------------------------------
def kiem_van_tay():
    """Bảng 3.5 của đồ án: các giá trị phải khớp từng bit giữa máy tính và ESP32."""
    T0 = C.hmac_tag(0, 0)
    p0 = C.perm_C_from_tag(T0)
    p12345 = C.perm_C(12345)
    dong = [
        ("T[0…7] với n = 0, r = 0", "9f 0c d9 b9 40 97 fe 49", T0[:8].hex(" ")),
        ("π[0…7] với n = 0", "109 1171 266 101 493 889 1106 419", " ".join(map(str, p0[:8]))),
        ("FNV-1a 64 của π với n = 0", "0xE4D8CAD02C139271", "0x%016X" % C.perm_fnv(p0)),
        ("FNV-1a 64 của π với n = 12345", "0x77CDD08E5E99A22D", "0x%016X" % C.perm_fnv(p12345)),
        ('CRC-16/CCITT-FALSE("123456789")', "0x29B1", "0x%04X" % C.crc16_ccitt_false(b"123456789")),
    ]
    return [dict(ten=a, ky_vong=b, tinh_duoc=c, khop=(b == c)) for a, b, c in dong]


def tu_kiem_ben_nhan():
    """Chạy nguyên hàm tu_kiem_tra() của tn16_receiver.py, giữ lại phần in ra."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        ma = R.tu_kiem_tra()
    return ma == 0, buf.getvalue().strip()


# ----------------------------------------------------------------------------
def ten_an_toan(i, ten):
    goc = unicodedata.normalize("NFKD", Path(ten).stem).encode("ascii", "ignore").decode()
    goc = re.sub(r"[^A-Za-z0-9_-]+", "_", goc).strip("_").lower() or "anh"
    return "%02d_%s" % (i + 1, goc[:40])


def chuan_bi_anh(duong_dan):
    """Đưa ảnh về 320x240 xám. Ảnh đã đúng 320x240 thì giữ nguyên từng điểm ảnh."""
    im = Image.open(duong_dan)
    im = ImageOps.exif_transpose(im)
    kich_thuoc_goc = im.size
    if im.mode != "L":
        im = im.convert("L")
    giu_nguyen = im.size == (C.W, C.H)
    if not giu_nguyen:
        im = ImageOps.fit(im, (C.W, C.H), method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    return im, kich_thuoc_goc, giu_nguyen


def nen_jpeg(raw, q):
    """Cùng tham số với phần tự kiểm của tn16_receiver.py."""
    b = io.BytesIO()
    Image.frombytes("L", (C.W, C.H), bytes(raw)).save(b, "JPEG", quality=q, optimize=False, subsampling=0)
    return b.getvalue()


def dong_goi_ban_tin(jpg, n, r, q):
    """Giống do_one_frame() của firmware: FLAGS, tiêu đề 12 byte, CRC trên 10 byte đầu + JPEG."""
    if len(jpg) > 65535:
        raise ValueError("JPEG %d B vượt quá trường LEN 16 bit" % len(jpg))
    flags = (PERM_MODE_PPC & 0x03) | ((Q_IDX[q] & 0x07) << 2)
    h = C.build_header(flags, r, n, len(jpg), 0)
    crc = C.crc16_ccitt_false(h[:10] + jpg)
    return bytes(C.build_header(flags, r, n, len(jpg), crc)) + jpg


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def ssim(a, b):
    try:
        from skimage.metrics import structural_similarity
    except ImportError:
        return None
    return float(structural_similarity(a, b, data_range=255))


def tieu_de_hex(msg):
    h = msg[:12]
    return {
        "magic": h[0:1].hex(), "ver": h[1:2].hex(), "flags": h[2:3].hex(), "r": h[3:4].hex(),
        "counter": h[4:8].hex(" "), "len": h[8:10].hex(" "), "crc": h[10:12].hex(" "),
        "jpeg_dau": msg[12:20].hex(" "),
    }


def luu_png(arr, duong_dan):
    Image.fromarray(arr).save(duong_dan, "PNG", optimize=True)


# ----------------------------------------------------------------------------
def xu_ly_anh(i, tep, n, q, r):
    ten = ten_an_toan(i, tep.name)
    im, kt_goc, giu = chuan_bi_anh(tep)
    goc = np.array(im, dtype=np.uint8)
    raw = goc.tobytes()

    # ---- bên gửi ----
    T = C.hmac_tag(n, r)
    perm = C.perm_C_from_tag(T)
    raw_xao = bytes(C.permute_blocks(raw, perm))
    jpg_ma = nen_jpeg(raw_xao, q)
    jpg_thuong = nen_jpeg(raw, q)                 # đối chứng C0: nén mà không mã hóa
    ban_tin = dong_goi_ban_tin(jpg_ma, n, r, q)

    # ---- bên nhận: đúng mã của tn16_receiver.py ----
    ok, ly_do, info = R.parse_frame(ban_tin)
    if not ok:
        raise RuntimeError("bên nhận từ chối bản tin %s: %s" % (tep.name, ly_do))
    anh_ma, khoi_phuc = R.decode_frame(info)

    # ---- kiểm: khôi phục phải trùng từng byte với JPEG không mã hóa cùng Q ----
    tham_chieu = np.array(Image.open(io.BytesIO(jpg_thuong)).convert("L"), dtype=np.uint8)
    byte_lech = int(np.count_nonzero(khoi_phuc != tham_chieu))

    for thu in ("goc", "ma", "khoi_phuc", "thuong", "ban_tin"):
        (THU_DATA / thu).mkdir(parents=True, exist_ok=True)
    luu_png(goc, THU_DATA / "goc" / (ten + ".png"))
    (THU_DATA / "ma" / (ten + ".jpg")).write_bytes(jpg_ma)
    (THU_DATA / "thuong" / (ten + ".jpg")).write_bytes(jpg_thuong)
    luu_png(np.asarray(khoi_phuc, dtype=np.uint8), THU_DATA / "khoi_phuc" / (ten + ".png"))
    (THU_DATA / "ban_tin" / (ten + ".bin")).write_bytes(ban_tin)

    s_thuong, s_ma = len(jpg_thuong), len(jpg_ma)
    ma_arr = np.asarray(anh_ma, dtype=np.uint8)
    gia_tri_ssim = ssim(goc, ma_arr)
    return {
        "loai": "mo_phong",
        "ten": ten, "tep_goc": tep.name,
        "kich_thuoc_goc": list(kt_goc), "giu_nguyen_320x240": giu,
        "n": n, "r": r, "q": q,
        "T_dau16": T[:16].hex(" "),
        "pi_dau8": perm[:8],
        "jpeg_thuong_B": s_thuong, "jpeg_ma_B": s_ma,
        "delta_B": s_ma - s_thuong,
        "delta_pt": round((s_ma - s_thuong) / s_thuong * 100, 2),
        "bit_moi_khoi": round((s_ma - s_thuong) * 8 / C.NBLK, 2),
        "ban_tin_B": len(ban_tin),
        "tieu_de": tieu_de_hex(ban_tin),
        "crc_dung": True,
        "byte_lech_khi_khoi_phuc": byte_lech,
        "trung_tung_byte": byte_lech == 0,
        "psnr_ma_dB": round(psnr(goc, ma_arr), 3),
        "ssim_ma": None if gia_tri_ssim is None else round(gia_tri_ssim, 4),
        "tep": {
            "goc": "data/goc/%s.png" % ten, "ma": "data/ma/%s.jpg" % ten,
            "khoi_phuc": "data/khoi_phuc/%s.png" % ten, "thuong": "data/thuong/%s.jpg" % ten,
            "ban_tin": "data/ban_tin/%s.bin" % ten,
        },
    }


def xu_ly_ban_tin_that(i, tep):
    """Bản tin .bin bắt thật từ MQTT của ESP32-CAM: chỉ có bên nhận, không có ảnh gốc."""
    ten = "esp32_%02d_%s" % (i + 1, re.sub(r"[^A-Za-z0-9_-]+", "_", tep.stem)[:40])
    msg = tep.read_bytes()
    ok, ly_do, info = R.parse_frame(msg)
    muc = {"loai": "esp32_that", "ten": ten, "tep_goc": tep.name, "ban_tin_B": len(msg), "crc_dung": ok}
    if not ok:
        muc["loi"] = ly_do
        return muc
    anh_ma, khoi_phuc = R.decode_frame(info)
    for thu in ("ma", "khoi_phuc", "ban_tin"):
        (THU_DATA / thu).mkdir(parents=True, exist_ok=True)
    (THU_DATA / "ma" / (ten + ".jpg")).write_bytes(info["jpg"])
    luu_png(np.asarray(khoi_phuc, dtype=np.uint8), THU_DATA / "khoi_phuc" / (ten + ".png"))
    (THU_DATA / "ban_tin" / (ten + ".bin")).write_bytes(msg)
    muc.update({
        "n": info["counter"], "r": info["r"], "perm_mode": info["perm_mode"], "q_idx": info["q_idx"],
        "jpeg_ma_B": info["length"], "tieu_de": tieu_de_hex(msg),
        "tep": {"ma": "data/ma/%s.jpg" % ten, "khoi_phuc": "data/khoi_phuc/%s.png" % ten,
                "ban_tin": "data/ban_tin/%s.bin" % ten},
    })
    return muc


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--q", type=int, default=20, choices=sorted(Q_IDX), help="chất lượng JPEG (đồ án chốt Q = 20)")
    ap.add_argument("--n0", type=int, default=0, help="số thứ tự khung của ảnh đầu tiên")
    ap.add_argument("--r", type=int, default=0, help="tham số r (PP-C luôn dùng 0)")
    a = ap.parse_args()

    van_tay = kiem_van_tay()
    for v in van_tay:
        print("# VÂN TAY %-34s %s" % (v["ten"], "KHỚP" if v["khop"] else "LỆCH: %s != %s" % (v["tinh_duoc"], v["ky_vong"])))
    if not all(v["khop"] for v in van_tay):
        sys.exit("DỪNG: vân tay lệch Bảng 3.5 — ma_do_an/cpe_core.py không còn đúng đặc tả.")
    ok_nhan, log_nhan = tu_kiem_ben_nhan()
    print(log_nhan)
    if not ok_nhan:
        sys.exit("DỪNG: tự kiểm của tn16_receiver.py không đạt.")

    if THU_DATA.exists():
        shutil.rmtree(THU_DATA)
    THU_DATA.mkdir(parents=True)

    tep_anh = sorted([p for p in THU_ANH.iterdir() if p.suffix.lower() in DUOI_ANH], key=lambda p: p.name.lower()) if THU_ANH.exists() else []
    tep_bin = sorted([p for p in THU_BANTIN_THAT.iterdir() if p.suffix.lower() == ".bin"], key=lambda p: p.name.lower()) if THU_BANTIN_THAT.exists() else []

    muc = []
    for i, tep in enumerate(tep_anh):
        t0 = time.perf_counter()
        m = xu_ly_anh(i, tep, a.n0 + i, a.q, a.r)
        muc.append(m)
        print("  %-28s n=%-6d %5d B -> %5d B  (%+.2f %%)  trùng từng byte: %s  [%.0f ms]" % (
            tep.name[:28], m["n"], m["jpeg_thuong_B"], m["jpeg_ma_B"], m["delta_pt"],
            "CÓ" if m["trung_tung_byte"] else "KHÔNG (%d byte lệch)" % m["byte_lech_khi_khoi_phuc"],
            (time.perf_counter() - t0) * 1000))
    for i, tep in enumerate(tep_bin):
        m = xu_ly_ban_tin_that(i, tep)
        muc.append(m)
        print("  [ESP32] %-20s %s" % (tep.name[:20], "CRC đúng, n=%d" % m["n"] if m["crc_dung"] else "HỎNG: %s" % m.get("loi")))

    mo_phong = [m for m in muc if m["loai"] == "mo_phong"]
    tong_hop = {"so_anh": len(mo_phong), "so_ban_tin_that": len(tep_bin)}
    if mo_phong:
        d = [m["delta_pt"] for m in mo_phong]
        tong_hop.update({
            "trung_tung_byte": sum(m["trung_tung_byte"] for m in mo_phong),
            "delta_pt_trung_vi": round(statistics.median(d), 2),
            "delta_pt_min": min(d), "delta_pt_max": max(d),
            "delta_B_trung_vi": statistics.median([m["delta_B"] for m in mo_phong]),
        })

    try:
        import PIL
        pil = PIL.__version__
    except Exception:
        pil = "?"
    du_lieu = {
        "sinh_luc": time.strftime("%Y-%m-%d %H:%M:%S"),
        "tham_so": {"q": a.q, "n0": a.n0, "r": a.r, "khoa": "khóa thử 00 01 … 1f (K_MASTER_TEST)",
                    "bo_nen": "Pillow %s (libjpeg-turbo), optimize=False" % pil,
                    "python": sys.version.split()[0], "numpy": np.__version__},
        "van_tay": van_tay,
        "tu_kiem_ben_nhan": {"dat": ok_nhan, "nhat_ky": log_nhan},
        "tong_hop": tong_hop,
        "anh": muc,
    }
    (THU_DATA / "manifest.js").write_text(
        "/* Tệp sinh tự động bởi tools/dung_trang.py — không sửa tay. */\nwindow.CPE_DATA = "
        + json.dumps(du_lieu, ensure_ascii=False, indent=1) + ";\n", encoding="utf-8")
    print("# Xong: %d ảnh, %d bản tin thật -> data/manifest.js" % (len(mo_phong), len(tep_bin)))
    if mo_phong and tong_hop["trung_tung_byte"] != len(mo_phong):
        sys.exit("CẢNH BÁO: có ảnh không khôi phục trùng từng byte.")


if __name__ == "__main__":
    main()
