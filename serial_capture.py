#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口日志采集（不需要 TTY，可后台跑）
用法：
    python serial_capture.py COM3 serial.log [--reset]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
out = sys.argv[2] if len(sys.argv) > 2 else "serial.log"
do_reset = "--reset" in sys.argv

ser = serial.Serial(port, 115200, timeout=1)

if do_reset:
    # 拉 DTR/RTS 触发 ESP32-S3 复位，以便抓到完整启动日志
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    ser.dtr = True
    time.sleep(0.1)
    print("[capture] reset pulse sent", flush=True)

print("[capture] listening on %s -> %s" % (port, out), flush=True)

with open(out, "a", encoding="utf-8", errors="replace") as f:
    f.write("\n===== capture start %s =====\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    f.flush()
    while True:
        try:
            raw = ser.readline()
        except Exception as e:  # 串口拔出 / 被占用
            print("[capture] read error: %r" % (e,), flush=True)
            break
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        print(line, flush=True)
        f.write(line + "\n")
        f.flush()
