#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成墨水屏 UI 资源头文件：中文点阵、大号数字、时钟数字、ASCII、天气图标。

用法: python tools/gen_ui_assets.py [选项]
输出: include/ui_assets.h

屏幕只有白/黑/红三色，图标与图片先按 4x 超采样渲染出灰度过渡，
再用三色 Floyd-Steinberg 误差扩散抖动成 1-bit 双层位图，曲线/斜边
不再是一刀切的硬锯齿，靠点阵疏密模拟中间调，显示更顺滑。
字模仍走硬阈值二值化：文字要的是锐利，抖动会让笔画发毛。

常用选项：
  --no-dither        回退到旧的硬阈值二值化（生成结果应与旧文件完全一致）
  --ss N             超采样倍数，默认 4
  --strength F       误差扩散强度 0~1，默认 1.0（调小可减轻噪点）
  --compare [路径]   额外输出「抖动前 / 抖动后」放大对比图
  --output 路径      指定头文件输出位置（默认 include/ui_assets.h）

改动字库或图片后重跑本脚本即可，main.cpp 不需要动。
"""
import argparse
import json
import math
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "include", "ui_assets.h")

FONT_CN = r"C:\Windows\Fonts\simhei.ttf"
FONT_NUM = r"C:\Windows\Fonts\arialbd.ttf"
FONT_ASCII = r"C:\Windows\Fonts\consola.ttf"

# 屏幕只有黑白红，所有素材都按这三个"颜色"生成
BLACK = (0, 0, 0)
RED = (200, 30, 30)
WHITE = (255, 255, 255)

CN_CHARS = ("上海晴多云阴小中大雨雪夹雾霾风沙尘浮扬无网络最高低天气获取失败"
            "按任意键重试扫码配网后点加入南京西宁深圳玉树城市切换传图设置正在使用定位")


def load_city_chars():
    """城市名用字：从 tools/data/cities.json 取，保证列表里任意城市都能上屏。

    353 个城市名去重后约 380 字，每个 16x16 字模 32 字节，合计约 12KB。
    """
    path = os.path.join(ROOT, "tools", "data", "cities.json")
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return "".join(c["n"] for c in data["cities"])
    except (OSError, KeyError, ValueError) as e:
        print("  [警告] 读不到城市数据（%s），城市名可能显示不全" % e)
        return ""


CN_CHARS += load_city_chars()

W = H = 152                 # 页面逻辑尺寸（超采样前的坐标系）

# ---- 抖动参数（改这里或命令行覆盖，都会写进生成的头文件注释里）----
SS_DEFAULT = 4              # 超采样倍数：4x 足够让斜边有 4 级灰度
RENDER_SCALE = 1            # 当前渲染倍率，由 render_asset() 设置；绘制函数据此放大画布
STRENGTH_DEFAULT = 1.0      # 误差扩散强度，0 = 硬阈值，1 = 标准 FS
RED_FULL = 170.0            # 纯红(200,30,30)的红度 r-(g+b)/2，用作红度归一化上限
K_RED_SUPPRESS = 1.15       # 红墨本身偏暗，用红度抵消黑度，避免红色块里冒黑点
DOWNSAMPLE = Image.LANCZOS  # 4x 下采样：LANCZOS 更保小细节；Image.BOX 是纯面积平均


def uniq(s):
    seen, out = set(), []
    for c in s:
        if c not in seen:
            seen.add(c)
            out.append(c)
    return out


def render_glyph(ch, size, font_path, cw, ch_, threshold=110):
    """把单个字符渲染成 cw x ch_ 的二值点阵，居中摆放。"""
    img = Image.new("L", (cw, ch_), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font_path, size)
    bbox = d.textbbox((0, 0), ch, font=f)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (cw - w) // 2 - bbox[0]
    y = (ch_ - h) // 2 - bbox[1]
    d.text((x, y), ch, font=f, fill=255)
    return img.point(lambda p: 255 if p >= threshold else 0)


def pack_mono(img, cw, ch_):
    """行优先、MSB 在左，每行按字节对齐。"""
    data = bytearray()
    for y in range(ch_):
        acc, n = 0, 0
        for x in range(cw):
            acc = (acc << 1) | (1 if img.getpixel((x, y)) else 0)
            n += 1
            if n == 8:
                data.append(acc)
                acc, n = 0, 0
        if n:
            data.append(acc << (8 - n))
    return bytes(data)


def classify(img):
    """RGB 图 -> (黑色层, 红色层)。抗锯齿灰边按阈值归到最近的颜色。"""
    w, h = img.size
    black, red = bytearray(), bytearray()
    for y in range(h):
        bb, rb, n = 0, 0, 0
        for x in range(w):
            r, g, b = img.getpixel((x, y))[:3]
            is_black = r < 100 and g < 100 and b < 100
            is_red = r > 130 and g < 120 and b < 120
            bb = (bb << 1) | (1 if is_black else 0)
            rb = (rb << 1) | (1 if is_red else 0)
            n += 1
            if n == 8:
                black.append(bb)
                red.append(rb)
                bb, rb, n = 0, 0, 0
        if n:
            black.append(bb << (8 - n))
            red.append(rb << (8 - n))
    return bytes(black), bytes(red)


# ---------------------------------------------------------------- 三色抖动
class ScaledDraw:
    """把 ImageDraw 的坐标、线宽、圆角整体放大 s 倍。

    超采样时绘制函数继续写 152x152 / 48x48 的原始坐标，不用逐个改；
    画完再缩回目标尺寸，硬边就变成带灰度过渡的连续色调图，抖动才有意义。
    """

    def __init__(self, draw, s):
        self.d = draw
        self.s = s

    def _xy(self, xy):
        s = self.s
        if len(xy) and isinstance(xy[0], (int, float)):
            return [v * s for v in xy]                 # [x0,y0,x1,y1] 或 [x,y,...]
        return [(p[0] * s, p[1] * s) for p in xy]      # [(x,y), (x,y), ...]

    def _kw(self, kw):
        kw = dict(kw)
        for key in ("width", "radius"):
            if key in kw and isinstance(kw[key], (int, float)):
                kw[key] = max(1, int(round(kw[key] * self.s)))
        return kw

    def _do(self, method, xy, kw):
        return getattr(self.d, method)(self._xy(xy), **self._kw(kw))

    def ellipse(self, xy, **kw):
        return self._do("ellipse", xy, kw)

    def rectangle(self, xy, **kw):
        return self._do("rectangle", xy, kw)

    def rounded_rectangle(self, xy, **kw):
        return self._do("rounded_rectangle", xy, kw)

    def line(self, xy, **kw):
        return self._do("line", xy, kw)

    def polygon(self, xy, **kw):
        return self._do("polygon", xy, kw)

    def arc(self, xy, start, end, **kw):
        return self._do("arc", xy, dict(kw, start=start, end=end))


def pack_states(states, w, h):
    """状态数组(0白/1黑/2红) -> (黑层, 红层)：行优先、MSB 在左、每行字节对齐。"""
    black, red = bytearray(), bytearray()
    for y in range(h):
        bb = rb = n = 0
        for x in range(w):
            v = states[y * w + x]
            bb = (bb << 1) | (1 if v == 1 else 0)
            rb = (rb << 1) | (1 if v == 2 else 0)
            n += 1
            if n == 8:
                black.append(bb)
                red.append(rb)
                bb = rb = n = 0
        if n:
            black.append(bb << (8 - n))
            red.append(rb << (8 - n))
    return bytes(black), bytes(red)


def dither_tricolor(img, strength=STRENGTH_DEFAULT, serpentine=True):
    """双通道三色 Floyd-Steinberg 误差扩散，返回与 classify() 同格式的双层位图。

    一个像素只能落一种墨水，所以先把颜色拆成两条互不串扰的量：
      黑度 K = 中性明暗，决定「黑 or 白」
      红度 R = 偏红程度，决定「红 or 白」
    各自做 1-bit 误差扩散，再按 黑 > 红 > 白 合成。这样灰色渐变的 R=0，
    绝不会泛红；红色块的 K 被红度抵消，也不会莫名冒出黑点。
    （若直接取「RGB 距离最近」的调色板色，中灰 128 反而离红色最近，
      整片黑白渐变会染成红色，这是三色屏抖动最常见的坑。）
    """
    w, h = img.size
    src = img.convert("RGB").load()
    K = [[0.0] * w for _ in range(h)]     # 黑度缓冲，误差在这里累积
    R = [[0.0] * w for _ in range(h)]     # 红度缓冲
    for y in range(h):
        krow, rrow = K[y], R[y]
        for x in range(w):
            r, g, b = src[x, y]
            redness = max(0.0, r - (g + b) * 0.5)
            luma = 0.299 * r + 0.587 * g + 0.114 * b
            k = (255.0 - luma) / 255.0 - redness / 255.0 * K_RED_SUPPRESS
            krow[x] = 0.0 if k < 0.0 else (1.0 if k > 1.0 else k)
            rrow[x] = 1.0 if redness >= RED_FULL else redness / RED_FULL

    states = bytearray(w * h)
    for y in range(h):
        krow, rrow = K[y], R[y]
        nk = K[y + 1] if y + 1 < h else None
        nr = R[y + 1] if y + 1 < h else None
        l2r = (y % 2 == 0) or not serpentine
        dx = 1 if l2r else -1
        xs = range(w) if l2r else range(w - 1, -1, -1)
        for x in xs:
            kv = 0.0 if krow[x] < 0.0 else (1.0 if krow[x] > 1.0 else krow[x])
            rv = 0.0 if rrow[x] < 0.0 else (1.0 if rrow[x] > 1.0 else rrow[x])
            kb = 1 if kv >= 0.5 else 0
            rb = 1 if rv >= 0.5 else 0
            states[y * w + x] = 1 if kb else (2 if rb else 0)

            ke = (kv - kb) * strength
            re = (rv - rb) * strength
            # FS 权重：右 7/16、左下 3/16、下 5/16、右下 1/16
            # 奇数行反向扫描时左右镜像，减轻单向误差造成的条纹/蚯蚓纹
            xr = x + dx
            if 0 <= xr < w:
                krow[xr] += ke * 0.4375
                rrow[xr] += re * 0.4375
            if nk is not None:
                nk[x] += ke * 0.3125
                nr[x] += re * 0.3125
                xd = x - dx
                if 0 <= xd < w:
                    nk[xd] += ke * 0.1875
                    nr[xd] += re * 0.1875
                if 0 <= xr < w:
                    nk[xr] += ke * 0.0625
                    nr[xr] += re * 0.0625

    return pack_states(states, w, h)


def render_asset(fn, w, h, ss=SS_DEFAULT, dither=True,
                 strength=STRENGTH_DEFAULT):
    """渲染一个图标/图片：超采样 -> 缩回目标尺寸出灰度 -> 三色抖动 -> 双层位图。

    绘制函数从模块级 RENDER_SCALE 读取倍率，因此不用逐个改签名和坐标。
    """
    global RENDER_SCALE
    RENDER_SCALE = ss if (dither and ss > 1) else 1
    img = fn()
    if RENDER_SCALE > 1:
        return dither_tricolor(img.resize((w, h), DOWNSAMPLE), strength)
    return classify(img)     # 旧管线（无超采样 + 硬阈值），用于回退与对比


# ---------------------------------------------------------------- 天气图标
IW = IH = 48


def icon_canvas():
    img = Image.new("RGB", (IW * RENDER_SCALE, IH * RENDER_SCALE), WHITE)
    return img, ScaledDraw(ImageDraw.Draw(img), RENDER_SCALE)


def cloud(d, cx, cy, fill=BLACK, s=1):
    d.ellipse([cx - 17 * s, cy - 7 * s, cx - 1 * s, cy + 9 * s], fill=fill)
    d.ellipse([cx - 9 * s, cy - 15 * s, cx + 9 * s, cy + 3 * s], fill=fill)
    d.ellipse([cx + 1 * s, cy - 7 * s, cx + 17 * s, cy + 9 * s], fill=fill)
    d.rectangle([cx - 17 * s, cy - 1 * s, cx + 17 * s, cy + 9 * s], fill=fill)


def icon_sun():
    img, d = icon_canvas()
    cx, cy = 24, 24
    d.ellipse([cx - 11, cy - 11, cx + 11, cy + 11], fill=RED)
    for i in range(8):
        a = math.radians(i * 45)
        x1, y1 = cx + 15 * math.cos(a), cy + 15 * math.sin(a)
        x2, y2 = cx + 21 * math.cos(a), cy + 21 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=3)
    return img


def icon_cloud_sun():
    img, d = icon_canvas()
    d.ellipse([28, 4, 46, 22], fill=RED)
    cloud(d, 20, 30)
    return img


def icon_cloud():
    img, d = icon_canvas()
    cloud(d, 24, 20)
    cloud(d, 28, 33, s=0.8)
    return img


def icon_rain():
    img, d = icon_canvas()
    cloud(d, 24, 16)
    for x in (12, 24, 36):
        d.line([x, 32, x - 6, 44], fill=BLACK, width=3)
    return img


def icon_snow():
    img, d = icon_canvas()
    cloud(d, 24, 16)
    for x, y in ((12, 34), (24, 30), (36, 34), (18, 42), (30, 42)):
        d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=BLACK)
    return img


def icon_fog():
    img, d = icon_canvas()
    for i, y in enumerate((16, 26, 36)):
        d.rounded_rectangle([8 + (i % 2) * 4, y, 40 - (i % 2) * 4, y + 4], radius=2, fill=BLACK)
    return img


def icon_thunder():
    img, d = icon_canvas()
    cloud(d, 24, 15)
    d.polygon([(26, 26), (16, 38), (23, 38), (19, 47), (33, 33), (25, 33)], fill=RED)
    return img


def icon_wind():
    img, d = icon_canvas()
    d.arc([4, 8, 40, 30], 200, 340, fill=BLACK, width=3)
    d.arc([8, 20, 44, 42], 20, 160, fill=BLACK, width=3)
    d.line([12, 26, 34, 26], fill=BLACK, width=3)
    return img


def icon_nonet():
    """断网：WiFi 扇形 + 红色斜杠"""
    img, d = icon_canvas()
    for r in (8, 16, 24):
        d.arc([24 - r, 34 - r, 24 + r, 34 + r], 225, 315, fill=BLACK, width=3)
    d.ellipse([20, 32, 28, 40], fill=BLACK)
    d.line([6, 6, 42, 42], fill=RED, width=4)
    return img


ICONS = [icon_sun, icon_cloud_sun, icon_cloud, icon_rain,
         icon_snow, icon_fog, icon_thunder, icon_wind, icon_nonet]
ICON_NAMES = ["sun", "cloud_sun", "cloud", "rain", "snow", "fog", "thunder", "wind", "nonet"]


# ---------------------------------------------------------------- 输出
def fmt_bytes(data, per_line=16, indent="    "):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ",".join("0x%02X" % b for b in chunk) + ",")
    return "\n".join(lines)


def to_rgb(black, red, w, h):
    """双层 1-bit 位图 -> RGB 预览图（与 main.cpp 的 drawDual 同款优先级）。"""
    img = Image.new("RGB", (w, h), WHITE)
    p = img.load()
    row_bytes = (w + 7) // 8
    for y in range(h):
        for x in range(w):
            idx = y * row_bytes + (x >> 3)
            m = 0x80 >> (x & 7)
            if black[idx] & m:
                p[x, y] = BLACK
            elif red[idx] & m:
                p[x, y] = RED
    return img


def make_compare(path, ss, strength):
    """输出「抖动前 / 抖动后」放大对比图，另附灰阶与红白渐变演示。"""
    global RENDER_SCALE
    samples = [("icon sun", icon_sun, IW, IH),
               ("icon rain", icon_rain, IW, IH),
               ("icon nonet", icon_nonet, IW, IH),
               ("icon snow", icon_snow, IW, IH)]
    pairs = []
    for name, fn, w, h in samples:
        RENDER_SCALE = 1
        before = classify(fn())
        RENDER_SCALE = ss
        after = dither_tricolor(fn().resize((w, h), DOWNSAMPLE), strength)
        pairs.append((name, before, after, w, h))

    # 连续色调渐变：硬阈值会切成两截，抖动则用点阵疏密还原过渡
    for name, target in (("gray ramp (W->K)", BLACK), ("red ramp (W->R)", RED)):
        ramp = Image.new("RGB", (W, H))
        rp = ramp.load()
        for y in range(H):
            for x in range(W):
                t = x / (W - 1)
                rp[x, y] = tuple(int(WHITE[c] * (1 - t) + target[c] * t)
                                 for c in range(3))
        pairs.append((name, classify(ramp), dither_tricolor(ramp, strength), W, H))
    RENDER_SCALE = 1

    cell, gap, label_h = 304, 16, 22
    cols, rows = 4, (len(pairs) + 1) // 2
    sheet = Image.new("RGB", (cols * cell + (cols + 1) * gap,
                              rows * (cell + label_h) + (rows + 1) * gap),
                      (245, 243, 238))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype(FONT_ASCII, 14)
    except Exception:
        font = ImageFont.load_default()

    for i, (name, before, after, w, h) in enumerate(pairs):
        r_, c_ = divmod(i, 2)
        for j, (tag, data) in enumerate((("OLD hard threshold", before),
                                         ("NEW tri-color dither", after))):
            cx = gap + (c_ * 2 + j) * (cell + gap)
            cy = gap + r_ * (cell + label_h + gap)
            view = to_rgb(data[0], data[1], w, h)
            zoom = 6 if w == IW else 2           # 整数倍放大，点阵才看得清
            view = view.resize((w * zoom, h * zoom), Image.NEAREST)
            sheet.paste(view, (cx + (cell - w * zoom) // 2, cy))
            d.text((cx, cy + cell + 4), "%s  /  %s" % (name, tag),
                   fill=(50, 50, 50), font=font)

    sheet.save(path)
    print("写入对比图", path, sheet.size)


def main():
    ap = argparse.ArgumentParser(description="生成墨水屏三色 UI 素材头文件")
    ap.add_argument("--no-dither", action="store_true",
                    help="回退到旧的硬阈值二值化（结果应与旧 ui_assets.h 完全一致）")
    ap.add_argument("--ss", type=int, default=SS_DEFAULT,
                    help="超采样倍数（默认 %d）" % SS_DEFAULT)
    ap.add_argument("--strength", type=float, default=STRENGTH_DEFAULT,
                    help="误差扩散强度 0~1（默认 %.2f）" % STRENGTH_DEFAULT)
    ap.add_argument("--compare", nargs="?", const=os.path.join(ROOT, "tools", "preview_dither.png"),
                    help="额外输出抖动前后对比图（默认 tools/preview_dither.png）")
    ap.add_argument("--output", default=OUT, help="头文件输出路径")
    args = ap.parse_args()

    if args.compare:
        make_compare(args.compare, args.ss, args.strength)

    opts = dict(dither=not args.no_dither, ss=args.ss, strength=args.strength)
    out = []
    out.append("// 本文件由 tools/gen_ui_assets.py 自动生成，请勿手工编辑。")
    if opts["dither"]:
        out.append("// 图标/图片：%dx 超采样 + 白/黑/红三色 Floyd-Steinberg 误差扩散抖动"
                   " (strength=%.2f)；字模保持硬阈值。" % (args.ss, args.strength))
    else:
        out.append("// 图标/图片：硬阈值二值化（未启用抖动）。")
    out.append("#pragma once")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define ICON_COUNT  %d" % len(ICONS))
    out.append("")

    # 中文 16x16
    cn = uniq(CN_CHARS)
    out.append("// %d 个 16x16 中文字模，行优先、MSB 在左" % len(cn))
    for i, ch in enumerate(cn):
        bmp = pack_mono(render_glyph(ch, 12, FONT_CN, 16, 16), 16, 16)
        out.append("static const uint8_t CN_%02d[32] = {" % i)
        out.append(fmt_bytes(bmp))
        out.append("};")
    out.append("struct CnGlyph { const char* utf8; const uint8_t* bmp; };")
    out.append("static const CnGlyph CN_GLYPHS[] = {")
    for i, ch in enumerate(cn):
        out.append('    {"%s", CN_%02d},' % (ch, i))
    out.append("};")
    out.append("static const int CN_GLYPH_COUNT = %d;" % len(cn))
    out.append("")

    # 32x48 大号数字：两位温度 + 度符号要挤进 152 宽的右半屏
    digits = "0123456789"
    out.append("#define BIG_DIGIT_W 32")
    out.append("#define BIG_DIGIT_H 48")
    out.append("// 32x48 大号数字，每行 4 字节")
    for i, ch in enumerate(digits):
        bmp = pack_mono(render_glyph(ch, 44, FONT_NUM, 32, 48), 32, 48)
        out.append("static const uint8_t BIG_%d[192] = {" % i)
        out.append(fmt_bytes(bmp))
        out.append("};")
    out.append("static const uint8_t* const BIG_DIGITS[10] = {%s};"
               % ",".join("BIG_%d" % i for i in range(10)))
    out.append("")

    # 16x24 时钟数字：天气页顶部显示 HH:MM
    clock_chars = "0123456789:-"
    out.append("#define CLOCK_W 16")
    out.append("#define CLOCK_H 24")
    out.append("// 16x24 时钟数字/冒号/负号，每行 2 字节")
    for i, ch in enumerate(clock_chars):
        bmp = pack_mono(render_glyph(ch, 23, FONT_NUM, 16, 24), 16, 24)
        out.append("static const uint8_t CLOCK_%02d[48] = {" % i)
        out.append(fmt_bytes(bmp))
        out.append("};")
    out.append("struct ClockGlyph { char ch; const uint8_t* bmp; };")
    out.append("static const ClockGlyph CLOCK_GLYPHS[] = {")
    for i, ch in enumerate(clock_chars):
        out.append("    {'%s', CLOCK_%02d}," % (ch, i))
    out.append("};")
    out.append("static const int CLOCK_GLYPH_COUNT = %d;" % len(clock_chars))
    out.append("")

    # ASCII 8x16
    ascii_chars = "0123456789/:. -%"  # % 给电池百分比用，追加在末尾不打乱已有下标
    out.append("// 8x16 ASCII，每行 1 字节")
    for i, ch in enumerate(ascii_chars):
        bmp = pack_mono(render_glyph(ch, 13, FONT_ASCII, 8, 16), 8, 16)
        out.append("static const uint8_t ASC_%02d[16] = {" % i)
        out.append(fmt_bytes(bmp, per_line=8))
        out.append("};")
    out.append("struct AsciiGlyph { char ch; const uint8_t* bmp; };")
    out.append("static const AsciiGlyph ASCII_GLYPHS[] = {")
    for i, ch in enumerate(ascii_chars):
        out.append("    {'%s', ASC_%02d}," % ("\\0" if ch == "\0" else ch, i))
    out.append("};")
    out.append("static const int ASCII_GLYPH_COUNT = %d;" % len(ascii_chars))
    out.append("")

    # 天气图标 48x48
    out.append("// 48x48 天气图标，黑色层 + 红色层")
    for name, fn in zip(ICON_NAMES, ICONS):
        b, r = render_asset(fn, IW, IH, **opts)
        out.append("static const uint8_t ICON_%s_B[288] = {" % name.upper())
        out.append(fmt_bytes(b))
        out.append("};")
        out.append("static const uint8_t ICON_%s_R[288] = {" % name.upper())
        out.append(fmt_bytes(r))
        out.append("};")
    out.append("struct IconData { const uint8_t* black; const uint8_t* red; };")
    out.append("static const IconData ICONS[ICON_COUNT] = {")
    for name in ICON_NAMES:
        out.append("    {ICON_%s_B, ICON_%s_R}," % (name.upper(), name.upper()))
    out.append("};")
    out.append("")

    text = "\n".join(out) + "\n"
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(text)
    print("写入 %s (%d 字节)" % (args.output, len(text.encode("utf-8"))))

    # 预览几个字模，确认字形没糊
    for ch in "上海晴":
        img = render_glyph(ch, 12, FONT_CN, 16, 16)
        print("\n[%s]" % ch)
        for y in range(16):
            print("  " + "".join("#" if img.getpixel((x, y)) else "." for x in range(16)))


if __name__ == "__main__":
    main()
