#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TN16 — BÊN NHẬN CHÍNH THỨC (chạy trên PC, cạnh broker mosquitto).

  python3 tn16_receiver.py --host 127.0.0.1 --port 1883 --dev esp01 --out anh_nhan
  python3 tn16_receiver.py --tu-kiem-tra          # tự kiểm tra, không cần board

Nhiệm vụ:
  1. Nhận cpe/<id>/frame, kiểm CRC-16/CCITT-FALSE, tách header 12 byte.
  2. Sinh lại hoán vị PP-C từ counter_n + r (KHÔNG cần trao đổi gì thêm).
  3. Giải nén JPEG, hoán vị nghịch, lưu PNG.
  4. Trả lời cpe/<id>/echoback ba chặng để ESP32 đo độ trễ khứ hồi:
        stage 0 — ngay khi nhận đủ khung (đã tải ~3 KB)
        stage 1 — sau khi giải mã xong
        stage 2 — khứ hồi của bản tin echo 4 byte (RTT MẠNG THUẦN)
  5. Phát hiện MẤT KHUNG bằng bước nhảy của counter_n; ghi CSV.
"""
import argparse, struct, time, os, sys, io, csv, socket
import numpy as np
from PIL import Image
import cpe_core as C

MAGIC, VER = 0x43, 0x01

def parse_frame(pl):
    """Trả (ok, lydo, dict). KHÔNG ném ngoại lệ — bên nhận phải sống sót mọi bản tin."""
    if len(pl) < 13:                       return False, "qua_ngan", None
    if pl[0] != MAGIC:                     return False, "sai_magic", None
    if pl[1] != VER:                       return False, "sai_ver", None
    flags, r = pl[2], pl[3]
    cn  = struct.unpack(">I", pl[4:8])[0]
    ln  = struct.unpack(">H", pl[8:10])[0]
    crc = struct.unpack(">H", pl[10:12])[0]
    jpg = pl[12:]
    if len(jpg) != ln:                     return False, "lech_do_dai", None
    if C.crc16_ccitt_false(bytes(pl[:10]) + bytes(jpg)) != crc:
        return False, "crc_hong", None
    return True, "", dict(flags=flags, r=r, counter=cn, length=ln, jpg=bytes(jpg),
                          perm_mode=flags & 0x03, q_idx=(flags >> 2) & 0x07)

def decode_frame(info):
    """Giải nén JPEG rồi hoán vị NGHỊCH -> ảnh khôi phục 320x240."""
    arr = np.array(Image.open(io.BytesIO(info["jpg"])).convert("L"), dtype=np.uint8)
    if arr.shape != (C.H, C.W):
        raise ValueError("kich thuoc %s khong phai 240x320" % (arr.shape,))
    if info["perm_mode"] == 0:
        return arr, arr
    T = C.hmac_tag(info["counter"], info["r"])
    p = C.perm_C_from_tag(T) if info["perm_mode"] == 2 else C.perm_A_from_u23(C.u23_from_tag(T))
    inv = C.inverse_perm(p)
    rec = np.frombuffer(bytes(C.permute_blocks(arr.tobytes(), inv)), dtype=np.uint8)
    return arr, rec.reshape(C.H, C.W)

# --------------------------------------------------------------------------
def run_live(a):
    import paho.mqtt.client as mqtt
    os.makedirs(a.out, exist_ok=True)
    t_frame  = "cpe/%s/frame"    % a.dev
    t_echo   = "cpe/%s/echo"     % a.dev
    t_back   = "cpe/%s/echoback" % a.dev
    t_stat   = "cpe/%s/stat"     % a.dev
    t_lwt    = "cpe/%s/lwt"      % a.dev
    fcsv = open(a.csv, "w", newline="")
    wr = csv.writer(fcsv)
    wr.writerow(["stt","counter","t_nhan_iso","len_ban_tin","len_jpeg","perm_mode","r",
                 "ket_qua","ms_giai_ma","buoc_counter","ghi_chu"])
    S = dict(n=0, ok=0, bad=0, lost=0, last=None, t0=None)

    def reply(cli, cn, stage):
        cli.publish(t_back, struct.pack(">IB", cn, stage), qos=0)

    def on_conn(cli, u, f, rc, props=None):
        cli.subscribe([(t_frame,0),(t_stat,0),(t_lwt,0)])
        print("# da ket noi broker, dang nghe %s" % t_frame, flush=True)

    def on_msg(cli, u, msg):
        if msg.topic in (t_stat, t_lwt):
            print("# %s <- %s" % (msg.topic, msg.payload[:120]), flush=True); return
        if msg.topic != t_frame: return
        tr = time.time()
        if S["t0"] is None: S["t0"] = tr
        S["n"] += 1
        ok, ly, info = parse_frame(msg.payload)
        if not ok:
            S["bad"] += 1
            wr.writerow([S["n"],"", time.strftime("%H:%M:%S"), len(msg.payload),"","","",
                         "HONG:"+ly,"","",""]); fcsv.flush()
            print("!! khung hong: %s (%d B)" % (ly, len(msg.payload)), flush=True); return
        cn = info["counter"]
        reply(cli, cn, 0)                       # chặng 0: vừa nhận đủ
        step = "" if S["last"] is None else cn - S["last"]
        if isinstance(step, int) and step > 1:
            S["lost"] += step - 1
            print("!! MAT %d khung (counter %d -> %d)" % (step-1, S["last"], cn), flush=True)
        S["last"] = cn
        note = ""
        t_d0 = time.perf_counter()
        try:
            _, rec = decode_frame(info)
            Image.fromarray(rec).save(os.path.join(a.out, "khoiphuc_%010d.png" % cn))
            S["ok"] += 1; res = "OK"
        except Exception as e:
            S["bad"] += 1; res = "HONG:giai_ma"; note = str(e)[:60]
        ms = (time.perf_counter() - t_d0) * 1000.0
        reply(cli, cn, 1)                       # chặng 1: giải mã xong
        wr.writerow([S["n"], cn, time.strftime("%H:%M:%S"), len(msg.payload), info["length"],
                     info["perm_mode"], info["r"], res, "%.3f" % ms, step, note]); fcsv.flush()
        if S["n"] % 10 == 0:
            dt = tr - S["t0"]
            print("# %4d khung | ok %d | hong %d | mat %d | %.3f khung/giay"
                  % (S["n"], S["ok"], S["bad"], S["lost"], S["n"]/dt if dt else 0), flush=True)

    def _nodelay(c, u, sk):
        # BAT BUOC. Ben nhan tra loi hai ban tin nho lien tiep (chang 0 roi
        # chang 1) tren cung mot socket; de Nagle bat thi ban tin thu hai phai
        # cho ACK cua ban tin thu nhat. Da do tren PC: chang 1 tu 54,43 ms
        # xuong 12,34 ms sau khi bat TCP_NODELAY. Khong lien quan den anh.
        try: sk.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except Exception: pass
    try:
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except Exception:
        cli = mqtt.Client()
    cli.on_socket_open = _nodelay
    cli.on_connect, cli.on_message = on_conn, on_msg
    cli.connect(a.host, a.port, 60)

    # --- CLIENT THU HAI, CHI DANH CHO TOPIC echo -----------------------------
    # BẮT BUỘC phải tách. paho xử lý bản tin trên MỘT luồng mạng theo thứ tự,
    # nên nếu trả lời echo bằng chính client đang giải mã ảnh thì chặng 2 sẽ
    # xếp hàng SAU cả việc giải mã và không còn đo được RTT mạng thuần.
    # (Đã đo trên PC: dùng chung client -> chặng 2 = 53,3 ms, chậm hơn cả
    #  chặng 1 = 11,0 ms; tách client -> chặng 2 xuống đúng mức RTT mạng.)
    try:
        cli2 = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except Exception:
        cli2 = mqtt.Client()
    cli2.on_socket_open = _nodelay
    def on_conn2(c, u, f, rc, props=None): c.subscribe(t_echo, 0)
    def on_msg2(c, u, m):
        if len(m.payload) >= 4:
            c.publish(t_back, struct.pack(">IB", struct.unpack(">I", m.payload[:4])[0], 2), 0)
    cli2.on_connect, cli2.on_message = on_conn2, on_msg2
    cli2.connect(a.host, a.port, 60); cli2.loop_start()
    print("# TN16 receiver — Ctrl-C de dung. CSV: %s" % a.csv, flush=True)
    try:
        cli.loop_forever()
    except KeyboardInterrupt:
        dt = (time.time() - S["t0"]) if S["t0"] else 0
        print("\n# ===== TONG KET BEN NHAN =====")
        print("# khung_nhan,%d" % S["n"]); print("# khung_giai_ma_dung,%d" % S["ok"])
        print("# khung_hong,%d" % S["bad"]); print("# khung_mat_suy_tu_counter,%d" % S["lost"])
        print("# thoi_gian_s,%.1f" % dt)
        print("# fps_ben_nhan,%.4f" % (S["n"]/dt if dt else 0))
        fcsv.close()

# --------------------------------------------------------------------------
def tu_kiem_tra():
    """Tự kiểm tra bên nhận mà KHÔNG cần board và KHÔNG cần mạng."""
    print("# TU KIEM TRA BEN NHAN TN16")
    img = np.frombuffer(C.xorshift32_image(), dtype=np.uint8).reshape(C.H, C.W)
    ok_all = True
    # dựng 20 bản tin đúng, cố tình bỏ 3 khung, cố tình làm hỏng 2 bản tin
    msgs, bo = [], {5, 6, 11}
    for n in range(20):
        if n in bo: continue
        T = C.hmac_tag(n, 0); p = C.perm_C_from_tag(T)
        permd = np.frombuffer(bytes(C.permute_blocks(img.tobytes(), p)),
                              dtype=np.uint8).reshape(C.H, C.W)
        buf = io.BytesIO(); Image.fromarray(permd).save(buf, "JPEG", quality=20,
                                                        optimize=False, subsampling=0)
        j = buf.getvalue()
        flags = 2 | (1 << 2)
        h = C.build_header(flags, 0, n, len(j), 0)
        crc = C.crc16_ccitt_false(h[:10] + j)
        msgs.append(bytes(C.build_header(flags, 0, n, len(j), crc)) + j)
    hong = set()
    for k in (3, 9):
        b = bytearray(msgs[k]); b[30] ^= 0xFF; msgs[k] = bytes(b); hong.add(k)

    n_ok = n_hong = n_mat = 0; last = None
    for k, m in enumerate(msgs):
        ok, ly, info = parse_frame(m)
        if not ok:
            n_hong += 1
            if k not in hong: print("  !! bat nham khung %d (%s)" % (k, ly)); ok_all = False
            continue
        if k in hong: print("  !! BO SOT khung hong %d" % k); ok_all = False
        if last is not None and info["counter"] - last > 1:
            n_mat += info["counter"] - last - 1
        last = info["counter"]
        decode_frame(info); n_ok += 1
    # 3 khung bi bo + 2 khung CRC hong (counter cua chung khong bao gio cap nhat) = 5
    print("  khung giai ma duoc      : %d  (ky vong 15)" % n_ok)
    print("  khung CRC bat duoc      : %d  (co tinh lam hong %d)" % (n_hong, len(hong)))
    print("  khung suy ra la MAT     : %d  (3 bo + 2 CRC hong = 5)" % n_mat)
    ok_all &= (n_ok == 15) and (n_hong == len(hong)) and (n_mat == 5)

    # PHEP THU QUAN TRONG NHAT: toan tuyen ben nhan (tach header -> kiem CRC ->
    # sinh lai hoan vi tu counter -> giai nen -> hoan vi nghich) phai cho ra
    # DUNG anh ma ben gui dinh gui, TRUNG TUNG BYTE, va KHONG duoc trao doi
    # them bat ky thong tin nao ngoai counter_n va r nam trong header.
    trung = 0; tong = 0
    for n in (0, 1, 7, 12345, 4294967295):
        T = C.hmac_tag(n, 0); p = C.perm_C_from_tag(T)
        permd = np.frombuffer(bytes(C.permute_blocks(img.tobytes(), p)),
                              dtype=np.uint8).reshape(C.H, C.W)
        b1 = io.BytesIO(); Image.fromarray(permd).save(b1, "JPEG", quality=20,
                                                       optimize=False, subsampling=0)
        j = b1.getvalue(); flags = 2 | (1 << 2)
        h = C.build_header(flags, 0, n, len(j), 0)
        msg = bytes(C.build_header(flags, 0, n, len(j),
                                   C.crc16_ccitt_false(h[:10] + j))) + j
        # ky vong: tinh doc lap, khong dung ham cua ben nhan
        de  = np.array(Image.open(io.BytesIO(j)).convert("L"), dtype=np.uint8)
        exp = np.frombuffer(bytes(C.permute_blocks(de.tobytes(), C.inverse_perm(p))),
                            dtype=np.uint8).reshape(C.H, C.W)
        ok, ly, info = parse_frame(msg)
        if not ok: print("  !! ban tin n=%d bi tu choi: %s" % (n, ly)); ok_all = False; continue
        _, rec = decode_frame(info)
        tong += 1; trung += int(np.array_equal(rec, exp))
    print("  toan tuyen trung tung byte: %d/%d  (counter 0,1,7,12345,2^32-1)" % (trung, tong))
    ok_all &= (trung == tong == 5)

    print("KET QUA TU KIEM TRA: %s" % ("DAT" if ok_all else "*** HONG ***"))
    return 0 if ok_all else 2

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1"); ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--dev", default="esp01"); ap.add_argument("--out", default="anh_nhan")
    ap.add_argument("--csv", default="TN16_ben_nhan.csv")
    ap.add_argument("--tu-kiem-tra", action="store_true")
    a = ap.parse_args()
    sys.exit(tu_kiem_tra() if a.tu_kiem_tra else (run_live(a) or 0))
