#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bat_ban_tin.py — lưu nguyên văn bản tin MQTT của ESP32-CAM thành tệp .bin.

    python tools/bat_ban_tin.py --host 127.0.0.1 --dev esp01 --so-khung 20

Chỉ ĐỌC topic cpe/<id>/frame và ghi payload ra ban_tin_esp32/. Không giải mã, không trả lời
echo, nên chạy song song với tn16_receiver.py được mà không làm lệch phép đo của nó.
"""
import argparse, os, sys
from pathlib import Path

import paho.mqtt.client as mqtt

THU = Path(__file__).resolve().parent.parent / "ban_tin_esp32"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--dev", default="esp01")
    ap.add_argument("--so-khung", type=int, default=20, help="dừng sau bấy nhiêu bản tin")
    a = ap.parse_args()
    THU.mkdir(exist_ok=True)
    topic = "cpe/%s/frame" % a.dev
    dem = {"n": 0}

    def on_connect(cli, *args):
        cli.subscribe(topic, 0)
        print("# đang nghe %s" % topic, flush=True)

    def on_message(cli, u, msg):
        dem["n"] += 1
        pl = bytes(msg.payload)
        so_khung = int.from_bytes(pl[4:8], "big") if len(pl) >= 8 else -1
        tep = THU / ("bantin_%03d_n%d.bin" % (dem["n"], so_khung))
        tep.write_bytes(pl)
        print("  %s  %d B" % (tep.name, len(pl)), flush=True)
        if dem["n"] >= a.so_khung:
            cli.disconnect()

    try:
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except Exception:
        cli = mqtt.Client()
    cli.on_connect, cli.on_message = on_connect, on_message
    cli.connect(a.host, a.port, 60)
    cli.loop_forever()
    print("# đã lưu %d bản tin vào %s" % (dem["n"], THU))


if __name__ == "__main__":
    sys.exit(main())
