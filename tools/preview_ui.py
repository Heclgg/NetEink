#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 ui_assets.h 的字模按 main.cpp 的坐标合成预览图，用于检查排版。

用法: python tools/preview_ui.py
输出: tools/preview.png
"""
import math
import os
import re
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H_PATH = os.path.join(ROOT, "include", "ui_assets.h")
OUT = os.path.join(ROOT, "tools", "preview.png")

W = H = 152
BLACK = (0, 0, 0)
RED = (200, 30, 30)
WHITE = (255, 255, 255)

SRC = open(H_PATH, encoding="utf-8").read()


def arr(name, size):
    m = re.search(r"static const uint8_t %s\[%d\] = \{(.*?)\};" % (name, size), SRC, re.S)
    if not m:
        raise SystemExit("找不到数组 " + name)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def load_cn():
    block = re.search(r"static const CnGlyph CN_GLYPHS\[\] = \{(.*?)\};", SRC, re.S).group(1)
    return {ch: arr("CN_" + idx, 32) for ch, idx in re.findall(r'\{"(.+?)", CN_(\d+)\}', block)}


def load_ascii():
    block = re.search(r"static const AsciiGlyph ASCII_GLYPHS\[\] = \{(.*?)\};", SRC, re.S).group(1)
    return {ch: arr("ASC_" + idx, 16) for ch, idx in re.findall(r"\{'(.)', ASC_(\d+)\}", block)}


CN = load_cn()
ASC = load_ascii()
BIG = {i: arr("BIG_%d" % i, 192) for i in range(10)}
ICON_NAMES = ["SUN", "CLOUD_SUN", "CLOUD", "RAIN", "SNOW", "FOG", "THUNDER", "WIND", "NONET"]
ICONS = {n: (arr("ICON_%s_B" % n, 288), arr("ICON_%s_R" % n, 288)) for n in ICON_NAMES}
CLOCK = {ch: arr("CLOCK_" + idx, 48)
         for ch, idx in re.findall(r"\{'(.)', CLOCK_(\d+)\}",
                                   re.search(r"static const ClockGlyph CLOCK_GLYPHS\[\] = \{(.*?)\};",
                                             SRC, re.S).group(1))}


def canvas():
    return Image.new("RGB", (W, H), WHITE)


def put(img, x, y, color):
    if 0 <= x < W and 0 <= y < H:
        img.putpixel((x, y), color)


def mono(img, x, y, data, w, h, color=BLACK):
    rb = (w + 7) // 8
    for row in range(h):
        for col in range(w):
            if data[row * rb + (col >> 3)] & (0x80 >> (col & 7)):
                put(img, x + col, y + row, color)


def dual(img, x, y, b, r, w, h):
    rb = (w + 7) // 8
    for row in range(h):
        for col in range(w):
            idx = row * rb + (col >> 3)
            m = 0x80 >> (col & 7)
            if b[idx] & m:
                c = BLACK
            elif r[idx] & m:
                c = RED
            else:
                c = WHITE
            put(img, x + col, y + row, c)


def clock_text(img, x, y, text):
    for ch in text:
        bmp = CLOCK.get(ch)
        if bmp:
            mono(img, x, y, bmp, 16, 24)
        else:
            ImageDraw.Draw(img).rectangle([x + 4, y + 9, x + 11, y + 14], fill=BLACK)
        x += 16


def cn(img, x, y, text, color=BLACK):
    for ch in text:
        bmp = CN.get(ch)
        if bmp:
            mono(img, x, y, bmp, 16, 16, color)
        else:
            ImageDraw.Draw(img).rectangle([x + 2, y + 2, x + 13, y + 13], fill=color)
        x += 16


def asc(img, x, y, text):
    for ch in text:
        bmp = ASC.get(ch)
        if bmp:
            mono(img, x, y, bmp, 8, 16)
        x += 8


def asc_c(img, x, y, text, color):
    for ch in text:
        bmp = ASC.get(ch)
        if bmp:
            mono(img, x, y, bmp, 8, 16, color)
        x += 8


def battery(img, right, y, pct):
    """电池图标 + 百分比，右对齐，对应 main.cpp 的 drawBattery"""
    ink = RED if pct <= 15 else BLACK
    txt = "%d%%" % pct
    tw = len(txt) * 8
    bx, by = right - tw - 6 - 20, y + 3
    d = ImageDraw.Draw(img)
    d.rectangle([bx, by, bx + 17, by + 9], fill=ink)
    d.rectangle([bx + 1, by + 1, bx + 16, by + 8], fill=WHITE)
    d.rectangle([bx + 18, by + 3, bx + 19, by + 6], fill=ink)
    w = (14 * pct + 50) // 100
    if w > 0:
        d.rectangle([bx + 2, by + 2, bx + 1 + w, by + 7], fill=ink)
    asc_c(img, right - tw, y, txt, ink)


def big(img, x, y, value):
    if value < 0:
        ImageDraw.Draw(img).rectangle([x + 2, y + 22, x + 17, y + 25], fill=BLACK)
        x += 20
        value = -value
    for ch in str(value):
        mono(img, x, y, BIG[int(ch)], 32, 48)
        x += 32
    return x


def ring(img, cx, cy, r, thick=1):
    for a in range(720):
        rad = a * 2 * math.pi / 720
        for t in range(thick):
            put(img, cx + int((r + t) * math.cos(rad)),
                cy + int((r + t) * math.sin(rad)), BLACK)


def hline(img, x0, x1, y):
    for x in range(x0, x1 + 1):
        put(img, x, y, BLACK)


def page_weather(temp=6, high=12, low=5, wtype="晴", date="11/22", clock="14:30", icon=0,
                 city="上海", batt=78):
    img = canvas()
    cn(img, 8, 2, city)
    asc(img, W - 8 - 40, 2, date)
    clock_text(img, (W - 5 * 16) // 2, 20, clock)
    hline(img, 6, W - 7, 47)
    b, r = ICONS[ICON_NAMES[icon]]
    dual(img, 10, 52, b, r, 48, 48)
    tx = 66 if (temp >= 10 or temp <= -10) else 80
    end = big(img, tx, 52, temp)
    ring(img, end + 8, 61, 4)
    cn(img, 10 + 24 - len(wtype) * 8, 104, wtype)
    if batt >= 0:
        battery(img, 146, 104, batt)
    hline(img, 6, W - 7, 124)
    cn(img, 10, 130, "最高")
    num = str(high)
    asc(img, 46, 130, num)
    ring(img, 46 + len(num) * 8 + 6, 135, 3)
    cn(img, 82, 130, "最低")
    num = str(low)
    asc(img, 118, 130, num)
    ring(img, 118 + len(num) * 8 + 6, 135, 3)
    return img


def page_nonet(title="无网络", city="上海", batt=78):
    img = canvas()
    cn(img, 8, 2, city)
    b, r = ICONS["NONET"]
    dual(img, 52, 22, b, r, 48, 48)
    w = len(title) * 16
    cn(img, (W - w) // 2, 80, title)
    hline(img, 46, 105, 104)
    w = len("按任意键重试") * 16
    cn(img, (W - w) // 2, 116, "按任意键重试")
    if batt >= 0:
        battery(img, 146, 2, batt)
    return img


def page_provisioning(ap_ssid="NetEink-A1B2", ap_pass="12345678"):
    img = canvas()
    w = len("扫码配网") * 16
    cn(img, (W - w) // 2, 1, "扫码配网")
    try:
        import segno
        qr = segno.make("WIFI:T:WPA;S:%s;P:%s;;" % (ap_ssid, ap_pass),
                        version=3, error="l", boost_error=False)
        matrix = qr.matrix
    except Exception as e:
        print("segno 不可用:", e)
        return img
    scale, quiet = 3, 4
    n = len(matrix)
    total = (n + quiet * 2) * scale
    ox, oy = (W - total) // 2, 19
    d = ImageDraw.Draw(img)
    for y in range(n):
        for x in range(n):
            if matrix[y][x]:
                d.rectangle([ox + (quiet + x) * scale, oy + (quiet + y) * scale,
                             ox + (quiet + x + 1) * scale - 1,
                             oy + (quiet + y + 1) * scale - 1], fill=BLACK)
    cn(img, (W - 96) // 2, 132, "扫码后点加入")
    return img


def page_link(ip="192.168.1.23", for_city=False, city="上海"):
    """设置页：标题 + 网址二维码 + 底部（城市名 / IP），对应 main.cpp 的 pageLink"""
    img = canvas()
    title = "扫码设置城市" if for_city else "扫码传图"
    w = len(title) * 16
    cn(img, (W - w) // 2, 1, title)
    try:
        import segno
        url = "http://%s/" % ip + ("#city" if for_city else "")
        qr = segno.make(url, version=3, error="l", boost_error=False)
        matrix = qr.matrix
    except Exception as e:
        print("segno 不可用:", e)
        return img
    scale, quiet = 3, 4
    n = len(matrix)
    total = (n + quiet * 2) * scale
    ox, oy = (W - total) // 2, 19
    d = ImageDraw.Draw(img)
    for y in range(n):
        for x in range(n):
            if matrix[y][x]:
                d.rectangle([ox + (quiet + x) * scale, oy + (quiet + y) * scale,
                             ox + (quiet + x + 1) * scale - 1,
                             oy + (quiet + y + 1) * scale - 1], fill=BLACK)
    if for_city:
        cw = len(city) * 16
        cn(img, (W - cw) // 2, 132, city)
    else:
        asc(img, (W - len(ip) * 8) // 2, 132, ip)
    return img


def main():
    items = [
        ("weather 上海 6deg sunny", page_weather(6, 12, 5, "晴", "11/22", "08:15", 0, "上海")),
        ("weather 南京 -3deg cloudy", page_weather(-3, 4, -7, "多云", "12/03", "14:30", 1, "南京")),
        ("weather 西宁 3deg overcast", page_weather(3, 9, -1, "阴", "11/22", "19:05", 2, "西宁")),
        ("weather 深圳 26deg rain", page_weather(26, 29, 22, "小雨", "11/22", "21:45", 3, "深圳")),
        ("weather 玉树 -8deg snow", page_weather(-8, 2, -14, "小雪", "11/22", "23:10", 4, "玉树")),
        ("weather 上海 5% battery", page_weather(6, 12, 5, "晴", "11/22", "08:15", 0, "上海", 5)),
        ("no network", page_nonet("无网络", "上海")),
        ("fetch failed", page_nonet("获取失败", "南京")),
        ("provisioning qr", page_provisioning()),
        ("photo upload qr", page_link()),
        ("city qr", page_link(for_city=True, city="上海")),
    ]

    cols, scale, gap, label_h = 3, 2, 14, 22
    cell_w = W * scale
    cell_h = H * scale + label_h
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell_w + (cols + 1) * gap,
                              rows * cell_h + (rows + 1) * gap), (245, 243, 238))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 13)
    except Exception:
        font = ImageFont.load_default()

    for i, (name, img) in enumerate(items):
        r_, c_ = divmod(i, cols)
        x = gap + c_ * (cell_w + gap)
        y = gap + r_ * (cell_h + gap)
        sheet.paste(img.resize((cell_w, H * scale), Image.NEAREST), (x, y))
        d.text((x, y + H * scale + 4), name, fill=(60, 60, 60), font=font)

    sheet.save(OUT)
    print("写入", OUT, sheet.size)


if __name__ == "__main__":
    main()
