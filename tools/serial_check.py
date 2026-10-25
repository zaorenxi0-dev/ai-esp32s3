#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
串口日志关键行过滤器
用法: python serial_check.py [端口] [秒数]
只打印有价值的行，过滤掉启动噪音与重复重启块。
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
DUR = int(sys.argv[2]) if len(sys.argv) > 2 else 30

# 值得关注的模式
KEYS = [
    'psram', 'PSRAM', 'SPIRAM',
    'Failed to init', 'abort()',
    'cpu_start',
    'wifi', 'WiFi', 'WIFI',
    'ota', 'OTA',
    'mqtt', 'MQTT',
    'websocket', 'Websocket',
    'Connecting', 'Connected', 'connection',
    'hello', 'Hello',
    'activation', 'Activation',
    'error', 'Error', 'ERROR', 'E (',
    'partition', 'boot: Loaded',
    'Free', 'heap',
    'your-server-ip',
]

# 噪音：这些行不打印
NOISE = [
    'esp_image: segment',
    'Writing at',
    'boot: ',
    'ESP-ROM:',
    'Build:',
    'rst:0x',
    'Saved PC:',
    'SPIWP:',
    'mode:DIO',
    'load:0x',
    'entry 0x',
    'ELF file SHA256',
    'Backtrace:',
    '0x40',
    'clk_drv',
    'gpio_drv',
]


def useful(line):
    s = line.strip()
    if not s:
        return False
    if any(n in s for n in NOISE):
        return False
    return any(k in s for k in KEYS)


def main():
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.5)
    except Exception as e:
        print('打开 %s 失败: %s' % (PORT, e))
        return 1

    print('=== 监听 %s，%d 秒（只显示关键行）===' % (PORT, DUR))
    t0 = time.time()
    buf = b''
    all_text = []
    last_emit = 0.0
    reboot_count = 0
    while time.time() - t0 < DUR:
        try:
            data = ser.read(8192)
        except Exception as e:
            print('读串口出错: %s' % e)
            break
        if not data:
            continue
        buf += data
        all_text.append(data.decode('utf-8', 'replace'))
        # 按行切，保留最后不完整的一段
        text = buf.decode('utf-8', 'replace')
        lines = text.split('\n')
        buf = lines[-1].encode('utf-8', 'replace')
        for ln in lines[:-1]:
            if 'Rebooting' in ln:
                reboot_count += 1
            if useful(ln):
                now = time.time() - t0
                # 限流：同内容 1 秒内只打一次，避免无限重启刷屏
                if now - last_emit >= 0.8:
                    print('[%5.1fs] %s' % (now, ln.strip()))
                    last_emit = now
    ser.close()

    full = ''.join(all_text)
    print('\n=== 统计 ===')
    print('监听时长: %d 秒' % DUR)
    for kw in ['psram', 'Failed to init external RAM', 'Connecting to endpoint',
               'Connected', 'hello', 'activation', 'MQTT', 'your-server-ip']:
        n = full.count(kw)
        print('  %-28s : %d' % (kw, n))
    print('\n提示：若结果里没有 "Connecting to endpoint"，说明还没走到联网阶段。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
