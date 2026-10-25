#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
固件运行日志抓取与关键事件分析
用法: python fw_log.py [端口] [秒数] [--reset]
  --reset  抓取前先用 DTR/RTS 复位设备（推荐，能抓到完整启动流程）

产出: fw_run.log（原始日志）
"""
import sys
import time
import os

try:
    import serial
except ImportError:
    print("缺少 pyserial，请用 ESP-IDF 的 python 运行本脚本")
    sys.exit(1)

PORT = 'COM3'
DUR = 45
DO_RESET = False
for a in sys.argv[1:]:
    if a == '--reset':
        DO_RESET = True
    elif a.isdigit():
        DUR = int(a)
    elif a.upper().startswith('COM'):
        PORT = a.upper()

OUT = 'fw_run.log'


def try_reset(ser):
    """拉低/拉高 DTR-RTS 触发 ESP32 复位（USB-Serial/JTAG 与 UART 桥都适用）"""
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        time.sleep(0.05)
        ser.setDTR(True)
        return True
    except Exception:
        return False


def main():
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.3)
    except Exception as e:
        print("打开 %s 失败: %s" % (PORT, e))
        print("提示: 若 idf.py monitor 正在运行，请先按 Ctrl+] 退出。")
        return 1

    ser.reset_input_buffer()
    if DO_RESET:
        ok = try_reset(ser)
        print("[复位] %s" % ("已发送复位信号" if ok else "复位失败，请手动按 RST"))
        time.sleep(0.4)

    print("=== 抓取 %s，%d 秒 -> %s ===" % (PORT, DUR, OUT))
    t0 = time.time()
    chunks = []
    with open(OUT, 'wb') as f:
        while time.time() - t0 < DUR:
            try:
                d = ser.read(4096)
            except Exception as e:
                print("读串口出错: %s" % e)
                break
            if d:
                f.write(d)
                f.flush()
                chunks.append(d)
    ser.close()

    raw = b''.join(chunks)
    text = raw.decode('utf-8', 'replace')
    print("=== 收到 %d 字节 ===" % len(raw))

    # ---- 关键事件提取 ----
    keys = [
        ('PSRAM', ['psram', 'PSRAM', 'Found 8MB', 'spiram']),
        ('WiFi', ['wifi: ', 'WiFi', 'connected to', 'got ip', 'IP=', 'sta ip']),
        ('OTA', ['ota', 'OTA', 'Ota:']),
        ('MQTT', ['Mqtt', 'mqtt', 'MQTT', 'Connecting to endpoint', 'Connected']),
        ('协议', ['hello', 'Hello', 'protocol', 'session']),
        ('MCP', ['mcp', 'MCP', 'tools/list', 'tool']),
        ('音频', ['opus', 'Opus', 'audio', 'Audio', 'UDP', 'udp']),
        ('错误', ['E (', 'error', 'Error', 'ERROR', 'abort', 'assert', 'fail']),
    ]
    lines = [ln for ln in text.split('\n') if ln.strip()]
    print("\n=== 命中统计（共 %d 行日志）===" % len(lines))
    for name, pats in keys:
        hit = [ln for ln in lines if any(p in ln for p in pats)]
        print("  %-6s : %d 行" % (name, len(hit)))

    print("\n=== 错误/异常行 ===")
    errs = [ln for ln in lines if ('E (' in ln or 'abort' in ln or 'ASSERT' in ln)]
    if errs:
        for ln in errs[:25]:
            print("  " + ln.strip()[:200])
    else:
        print("  无")

    print("\n=== 连接关键行 ===")
    important = ['Connecting to endpoint', 'Connected', 'connected', 'hello',
                 'Ota:', 'OTA', 'activation', 'MCP', 'mcp', 'got ip', 'IP=']
    shown = 0
    for ln in lines:
        if any(k in ln for k in important):
            print("  " + ln.strip()[:200])
            shown += 1
            if shown >= 40:
                break
    if shown == 0:
        print("  （未命中，可能设备已稳定运行在多轮心跳之间）")

    print("\n原始日志: %s" % os.path.abspath(OUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
