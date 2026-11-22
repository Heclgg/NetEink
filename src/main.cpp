/**
 *  墨水屏天气牌（时钟 + 任意城市天气）—— 自画 ESP32-S2 板
 *  文件：src/*.cpp 是实现，include/*.h 是声明（含脚本生成的 include/ui_assets.h、
 *        include/cities_data.h）
 * ----------------------------------------------------------------------------
 *  流程：
 *    上电 -> 连 WiFi -> NTP 对时 -> 拉当前城市天气（默认上海，可换 352 个城市）
 *         -> 绘制 152x152 三色界面 -> 刷新
 *         -> 之后每 15 分钟（对齐 :00/:15/:30/:45）对时 + 拉天气 + 刷屏
 *      |             |
 *      |             +-- 失败：断网图标 + "无网络"，时间显示 --:--
 *      +-- 成功：顶部城市/日期/时间，左侧天气图标，右侧大号温度，底部高低温
 *
 *  换城市：手机上选，见 include/city.h
 *    短按        -> 开设置页：屏上二维码，手机扫码后可选城市 / 用当前位置自动定位
 *    选完自动回天气页并刷新；城市编号存 NVS，重启后保持
 *    城市表 352 个（中国天气网编号 + 经纬度），按 IP 定位时就近匹配
 *
 *  按键（KEY1/GPIO1、KEY2/GPIO2）：
 *    长按 KEY1   -> 扫码传图：屏幕显示二维码，手机扫码上传照片；再长按 KEY1 退出
 *                   同一个网页里也能设城市，只是入口不同（扫码后自动滚到对应区块）
 *    长按 KEY2   -> 直接重放上一次传的图（存在 Flash 里，不联网、不用手机）；
 *                   还没传过图时和长按 KEY1 一样，落到扫码页；
 *                   屏上已经是照片时长按 = 回天气页（不重复刷同一张）
 *    看图页 / 设置页里短按不响应，避免误触把照片刷掉
 *
 *  传图：内置网页（src/photo_server.cpp），裁剪/缩放/三色抖动都在手机端完成，
 *        设备只收 5776 字节的双层点阵，不占 RAM 也不需要 JPEG 解码器。
 *        传图时暂停 15 分钟定时刷新，照片一直留在屏上，直到长按退出。
 *        收到的图会另存一份到 Flash（src/photo_store.cpp），断电也不丢，
 *        长按 KEY2 就能调出来，当电子相框用。
 *
 *  指示灯：板载 WS2812(GPIO9)，只有两种状态亮灯，其余一律熄灭（见 include/rgb_led.h）
 *    绿色闪三下 = 图片已经刷到屏上（传图成功 / 长按 KEY2 重放）
 *    橙色闪三下 = 配网二维码已就绪，等手机提交
 *    都是闪完就熄，不常亮
 *    刷屏中 / 联网 / 对时 / 取天气 / 天气页刷新：都不亮
 *    颜色 / 周期 / 余光下限都在 src/rgb_led.cpp 的 kPresets[] 一张表里
 *
 *  屏幕：微雪 1.54inch e-Paper (C)，152x152，黑/白/红三色
 *  素材：include/ui_assets.h 由 tools/gen_ui_assets.py 生成（改字/改图重跑脚本）
 *        图标/图片在生成端已做 4x 超采样 + 白/黑/红三色误差扩散抖动，
 *        用点阵疏密模拟中间调，边缘/渐变更顺滑；落到屏上仍是 1-bit 双层
 *        位图，MCU 端没有灰度可还原，所以这里只直出、不再抖动。
 *        字模保持硬阈值二值化，避免笔画被抖动打毛。
 *  日志：USB CDC(COM9) 与 UART0(GPIO43/44) 双路输出，见 include/log.h
 *        构建：pio run = 调试版（日志全开）
 *              pio run -e release = 发布版（日志与格式串全部编译掉，省 63KB Flash）
 */
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#include "epd1in54c.h"
#include "log.h"
#include "ui_assets.h"
#include "city.h"
#include "battery.h"
#include "qrcode.h"
#include "provisioning.h"
#include "photo_server.h"
#include "photo_store.h"
#include "rgb_led.h"

/* ============================================================================
 * 配置
 * WiFi 凭据不再写死在代码里：首次上电（或连不上）会开热点进入配网，
 * 凭据保存在 NVS（namespace: wifi），由 provisioning.cpp 管理。
 * ==========================================================================*/

// 上海 city code = 101020100；这个接口免 key、走 HTTP，S2 上没有 TLS 内存压力
#define WEATHER_BASE           "http://t.weather.itboy.net/api/weather/city/"

#define AP_PASSWORD            "12345678"   // 配网热点密码（WPA2 至少 8 位）

#define WIFI_TIMEOUT_MS        15000
#define HTTP_TIMEOUT_MS         8000
#define DEBOUNCE_MS               30
#define LONG_PRESS_MS           1500

#ifndef BOARD_RGB_PIN
#define BOARD_RGB_PIN              9
#endif
#ifndef BOARD_RGB_PEAK
#define BOARD_RGB_PEAK            255    // 闪烁峰值亮度 0-255：嫌亮就调小
#endif

#define PIN_BTN_A                  1   // KEY1
#define PIN_BTN_B                  2   // KEY2

/* ============================================================================
 * 日志：logf / logLine 见 log.h（双路输出，release 下编译为空）
 * ==========================================================================*/

/* ============================================================================
 * 板载 WS2812（GPIO9）：波形与「闪几次」在 rgb_led.cpp，这里只决定什么状态什么颜色
 * ----------------------------------------------------------------------------
 * 只有两个状态亮灯，而且都是「需要你看一眼」的静止状态：
 *   绿色 = 图片已经在屏上（传图成功 / 长按 KEY2 重放）
 *   橙色 = 配网二维码已就绪，等手机提交凭据
 * 刷屏期间（15~20s）、联网、对时、取天气、天气页定时刷新都不亮灯——
 * 这些过程屏上本来就有变化，半夜里多一盏灯只会碍事。
 * 熄灭走 rgbOff() 的淡出，约 0.38s 掉到全黑，不是硬切。
 * ==========================================================================*/

/* ============================================================================
 * 帧缓冲：黑白层 + 红色层，各 152*152/8 = 2888 字节
 * 位序：每行 19 字节，MSB 对应最左像素，行优先从上到下
 * ==========================================================================*/
static uint8_t g_black[EPD_WIDTH * EPD_HEIGHT / 8];
static uint8_t g_red[EPD_WIDTH * EPD_HEIGHT / 8];
static Epd g_epd;

enum Ink : uint8_t { INK_WHITE = 0, INK_BLACK = 1, INK_RED = 2 };

static void canvasClear() {
    // 这块屏的极性：位=1 表示「不着色」，0 才着色
    //   白 = (1,1)   黑 = (0,1)   红 = (1,0)
    // 依据 lib/EPD1in54C/src/imagedata.cpp 的示例图：两层的背景都是 0xFF。
    memset(g_black, 0xFF, sizeof(g_black));
    memset(g_red, 0xFF, sizeof(g_red));
}

static inline void putPixel(int x, int y, Ink ink) {
    if ((unsigned)x >= (unsigned)EPD_WIDTH || (unsigned)y >= (unsigned)EPD_HEIGHT) return;
    const int idx = y * (EPD_WIDTH / 8) + (x >> 3);
    const uint8_t mask = 0x80 >> (x & 7);
    if (ink == INK_BLACK) {           // 黑 = (0,1)
        g_black[idx] &= ~mask;
        g_red[idx] |= mask;
    } else if (ink == INK_RED) {      // 红 = (1,0)
        g_black[idx] |= mask;
        g_red[idx] &= ~mask;
    } else {                           // 白 = (1,1)
        g_black[idx] |= mask;
        g_red[idx] |= mask;
    }
}

static void fillRect(int x, int y, int w, int h, Ink ink) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            putPixel(x + i, y + j, ink);
}

static void hLine(int x0, int x1, int y, Ink ink) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) putPixel(x, y, ink);
}

/** 画单色 1-bit 位图（字模） */
static void drawMono(int x, int y, const uint8_t* bmp, int w, int h, Ink ink) {
    const int rowBytes = (w + 7) / 8;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            if (bmp[row * rowBytes + (col >> 3)] & (0x80 >> (col & 7)))
                putPixel(x + col, y + row, ink);
}

/** 画双色位图（图标 / 照片）：黑层优先，其次红层，其余留白 */
static void drawDual(int x, int y, const uint8_t* black, const uint8_t* red, int w, int h) {
    const int rowBytes = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const int idx = row * rowBytes + (col >> 3);
            const uint8_t mask = 0x80 >> (col & 7);
            if (black[idx] & mask)      putPixel(x + col, y + row, INK_BLACK);
            else if (red[idx] & mask)   putPixel(x + col, y + row, INK_RED);
            else                        putPixel(x + col, y + row, INK_WHITE);
        }
    }
}

/** 空心圆环，用于画温度单位 ° */
static void strokeCircle(int cx, int cy, int r, int thick, Ink ink) {
    for (int a = 0; a < 720; a++) {
        const float rad = a * 0.00872665f;   // 2*PI/720
        for (int t = 0; t < thick; t++) {
            putPixel(cx + (int)((r + t) * cosf(rad)),
                     cy + (int)((r + t) * sinf(rad)), ink);
        }
    }
}

/* ============================================================================
 * 文字：16x16 中文 + 8x16 ASCII + 32x48 大号数字
 * ==========================================================================*/
static int utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

static const uint8_t* findCn(const char* utf8) {
    for (int i = 0; i < CN_GLYPH_COUNT; i++)
        if (strcmp(CN_GLYPHS[i].utf8, utf8) == 0) return CN_GLYPHS[i].bmp;
    return nullptr;
}

static int cnWidth(const char* s) {
    int n = 0;
    while (*s) { s += utf8Len((unsigned char)*s); n++; }
    return n * 16;
}

static void drawCnText(int x, int y, const char* s, Ink ink) {
    while (*s) {
        const int n = utf8Len((unsigned char)*s);
        char ch[5] = {0};
        memcpy(ch, s, n);
        const uint8_t* bmp = findCn(ch);
        if (bmp) drawMono(x, y, bmp, 16, 16, ink);
        else     fillRect(x + 2, y + 2, 12, 12, ink);   // 缺字：画方块提示
        x += 16;
        s += n;
    }
}

static void drawCnCenter(int y, const char* s, Ink ink) {
    drawCnText((EPD_WIDTH - cnWidth(s)) / 2, y, s, ink);
}

static const uint8_t* findAscii(char c) {
    for (int i = 0; i < ASCII_GLYPH_COUNT; i++)
        if (ASCII_GLYPHS[i].ch == c) return ASCII_GLYPHS[i].bmp;
    return nullptr;
}

static void drawAsciiText(int x, int y, const char* s, Ink ink) {
    while (*s) {
        const uint8_t* bmp = findAscii(*s);
        if (bmp) drawMono(x, y, bmp, 8, 16, ink);
        x += 8;
        s++;
    }
}

/** 32x48 大号数字，返回绘制结束的 x 坐标 */
static int drawBigNumber(int x, int y, int value, Ink ink) {
    if (value < 0) {
        fillRect(x + 2, y + 22, 16, 4, ink);   // 负号
        x += 20;
        value = -value;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    for (char* p = buf; *p; p++) {
        if (*p >= '0' && *p <= '9')
            drawMono(x, y, BIG_DIGITS[*p - '0'], BIG_DIGIT_W, BIG_DIGIT_H, ink);
        x += BIG_DIGIT_W;
    }
    return x;
}

/** 16x24 时钟数字（顶部 HH:MM），缺字画短横占位 */
static const uint8_t* findClock(char c) {
    for (int i = 0; i < CLOCK_GLYPH_COUNT; i++)
        if (CLOCK_GLYPHS[i].ch == c) return CLOCK_GLYPHS[i].bmp;
    return nullptr;
}

static void drawClockText(int x, int y, const char* s, Ink ink) {
    while (*s) {
        const uint8_t* bmp = findClock(*s);
        if (bmp) drawMono(x, y, bmp, CLOCK_W, CLOCK_H, ink);
        else     fillRect(x + 4, y + 9, 8, 6, ink);
        x += CLOCK_W;
        s++;
    }
}

/* ============================================================================
 * 时间：NTP 对时 + 本地时间格式化
 * 时区固定中国标准时间（UTC+8、无夏令时），POSIX TZ 字符串写 "CST-8"。
 * ==========================================================================*/
static bool clockValid() {
    return time(nullptr) > 1600000000;    // 晚于 2020-09-13 才认为已同步
}

static void startClockSync() {
    configTzTime("CST-8", "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");
}

static bool waitClock(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (!clockValid() && millis() - t0 < timeoutMs) {
        rgbTick(); // 对时要等好几秒，灯的倒计时要继续走
        delay(20);
    }
    return clockValid();
}

static bool formatClock(char* out, size_t n) {
    if (!clockValid()) { snprintf(out, n, "--:--"); return false; }
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return true;
}

static void formatDate(char* out, size_t n) {
    if (!clockValid()) { snprintf(out, n, "--/--"); return; }
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(out, n, "%02d/%02d", tmv.tm_mon + 1, tmv.tm_mday);
}

/* ============================================================================
 * 城市：表和「当前是哪个」都在 include/city.h / src/city.cpp 里
 * 这里只管拉天气和往屏上画
 * ==========================================================================*/

/* ============================================================================
 * 天气数据
 * ==========================================================================*/
struct Weather {
    bool valid = false;
    int  temp  = 0;
    int  high  = 0;
    int  low   = 0;
    char city[16] = "上海";
    char type[24] = "多云";
    char date[8]  = "--/--";
};

static bool extractQuoted(const String& s, const char* key, char* out, size_t n, int from = 0) {
    String pat = String("\"") + key + "\":\"";
    int i = s.indexOf(pat, from);
    if (i < 0) return false;
    i += pat.length();
    int j = s.indexOf('"', i);
    if (j < 0) return false;
    strncpy(out, s.substring(i, j).c_str(), n - 1);
    out[n - 1] = '\0';
    return true;
}

static int firstInt(const char* s) {
    while (*s && *s != '-' && (*s < '0' || *s > '9')) s++;
    return atoi(s);
}

#if NETEINK_LOG
/**
 * 天气类型 -> 串口里的英文词。
 * 接口返回的是中文（"小雨"、"晴转多云"），串口只出英文，这里按键归类，
 * 关键字和 pickIcon() 一致，日志和图标说的是同一件事。
 * release 下所有日志被宏删掉，这个函数也跟着不编译，避免 unused 警告。
 */
static const char* weatherEn(const char* type)
{
    if (strstr(type, "雷")) return "thunder";
    if (strstr(type, "雪")) return "snow";
    if (strstr(type, "雨")) return "rain";
    if (strstr(type, "雾") || strstr(type, "霾")) return "fog";
    if (strstr(type, "风") || strstr(type, "沙") ||
        strstr(type, "尘") || strstr(type, "扬"))
        return "wind";
    if (strstr(type, "阴")) return "overcast";
    if (strstr(type, "多云")) return "cloudy";
    if (strstr(type, "晴")) return "clear";
    return "unknown";
}
#endif

static bool connectWiFi(const String& ssid, const String& pass) {
    logf("[WiFi] connecting %s ...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    // 联网期间不亮灯（只在配网就绪/图已上屏这两个时刻闪三下），rgbTick 仍要推，
    // 否则从配网橙色退出时会卡在半亮
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        rgbTick();
        delay(10);
    }

    if (WiFi.status() == WL_CONNECTED) {
        logf("[WiFi] connected %s  RSSI=%d\n",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }
    logLine("[WiFi] connect failed");
    return false;
}

static bool fetchWeather(Weather& w) {
    const CityRec& city = cityCurrent();
    char url[80];
    snprintf(url, sizeof(url), WEATHER_BASE "%09lu", (unsigned long)city.code);
    logf("[HTTP] GET code %09lu\n", (unsigned long)city.code);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
        logLine("[HTTP] begin failed");
        return false;
    }

    const int code = http.GET();
    if (code != 200) {
        logf("[HTTP] status %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    logf("[HTTP] got %u bytes\n", (unsigned)body.length());
    if (body.length() < 100) return false;

    char buf[64];

    // 当前温度 "wendu":"5.5"
    if (extractQuoted(body, "wendu", buf, sizeof(buf))) {
        w.temp = (int)lroundf(atof(buf));
    }

    // 预报字段都在 "forecast":[ 之后，避免取到 yesterday 的数据
    const int fc = body.indexOf("\"forecast\":[");
    if (fc < 0) return false;

    if (extractQuoted(body, "type", w.type, sizeof(w.type), fc)) {
        logf("[HTTP] weather %s\n", weatherEn(w.type));
    }
    if (extractQuoted(body, "high", buf, sizeof(buf), fc)) {
        w.high = firstInt(buf);
    }
    if (extractQuoted(body, "low", buf, sizeof(buf), fc)) {
        w.low = firstInt(buf);
    }

    // 顶层 "date":"20261120" -> 11/20
    if (extractQuoted(body, "date", buf, sizeof(buf)) && strlen(buf) >= 8) {
        snprintf(w.date, sizeof(w.date), "%c%c/%c%c", buf[4], buf[5], buf[6], buf[7]);
    }

    w.valid = true;
    strncpy(w.city, city.name, sizeof(w.city) - 1);
    logf("[HTTP] %s  %dC  high %d  low %d  %s\n",
         w.date, w.temp, w.high, w.low, weatherEn(w.type));
    return true;
}

/** 把中文天气映射到 9 个图标之一 */
static int pickIcon(const char* type) {
    if (strstr(type, "雷")) return 6;
    if (strstr(type, "雪")) return 4;
    if (strstr(type, "雨")) return 3;
    if (strstr(type, "雾") || strstr(type, "霾")) return 5;
    if (strstr(type, "风") || strstr(type, "沙") ||
        strstr(type, "尘") || strstr(type, "扬")) return 7;
    if (strstr(type, "阴")) return 2;
    if (strstr(type, "多云")) return 1;
    if (strstr(type, "晴")) return 0;
    return 2;
}

/* ============================================================================
 * 页面绘制
 * ==========================================================================*/
static int g_batteryMv = 0;
static int g_batteryPct = -1; // -1 = 还没采过

/**
 * 电池图标 + 百分比：右对齐到 right，纵向占 y..y+15（和一行 ASCII 同高）。
 * 低于 15% 用红色——三色屏上这是最直接的「该充电了」。
 */
static void drawBattery(int right, int y, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const Ink ink = (pct <= 15) ? INK_RED : INK_BLACK;

    char txt[8];
    snprintf(txt, sizeof(txt), "%d%%", pct);
    const int tw = (int)strlen(txt) * 8;

    // 外壳 18x10 + 正极 2x4，与 16px 高的文字居中对齐
    const int bx = right - tw - 6 - 20;
    const int by = y + 3;
    fillRect(bx, by, 18, 10, ink);
    fillRect(bx + 1, by + 1, 16, 8, INK_WHITE);
    fillRect(bx + 18, by + 3, 2, 4, ink);

    const int w = (14 * pct + 50) / 100; // 内侧 14px 按比例填
    if (w > 0) fillRect(bx + 2, by + 2, w, 6, ink);

    drawAsciiText(right - tw, y, txt, ink);
}

static void pageWeather(const Weather& w) {
    canvasClear();

    // 顶部：城市 + 日期（日期取本地时间，与时钟同源，不依赖天气接口）
    drawCnText(8, 2, w.city, INK_BLACK);
    char date[8];
    formatDate(date, sizeof(date));
    drawAsciiText(EPD_WIDTH - 8 - 40, 2, date, INK_BLACK);

    // 当前时间 HH:MM：16x24 数字，5 个字符共 80px，水平居中
    char hhmm[8];
    formatClock(hhmm, sizeof(hhmm));
    drawClockText((EPD_WIDTH - 5 * CLOCK_W) / 2, 20, hhmm, INK_BLACK);

    hLine(6, EPD_WIDTH - 7, 47, INK_BLACK);

    // 左侧天气图标 48x48
    const int icon = pickIcon(w.type);
    drawDual(10, 52, ICONS[icon].black, ICONS[icon].red, 48, 48);

    // 右侧大号温度：2 位数/负数从 x=66 起，1 位数从 x=80 起，视觉居中
    const int tempX = (w.temp >= 10 || w.temp <= -10) ? 66 : 80;
    const int endX = drawBigNumber(tempX, 52, w.temp, INK_BLACK);
    strokeCircle(endX + 8, 61, 4, 1, INK_BLACK);   // 温度单位 °

    // 天气文字放在图标正下方，与图标成一组
    drawCnText(10 + 24 - cnWidth(w.type) / 2, 104, w.type, INK_BLACK);

    // 电池：图标 + 百分比，右对齐在同一行的右端
    if (g_batteryPct >= 0) drawBattery(146, 104, g_batteryPct);

    // 底部：最高 / 最低
    hLine(6, EPD_WIDTH - 7, 124, INK_BLACK);

    char num[8];
    drawCnText(10, 130, "最高", INK_BLACK);
    snprintf(num, sizeof(num), "%d", w.high);
    int hx = 10 + 32 + 4;
    drawAsciiText(hx, 130, num, INK_BLACK);
    strokeCircle(hx + (int)strlen(num) * 8 + 6, 135, 3, 1, INK_BLACK);

    drawCnText(82, 130, "最低", INK_BLACK);
    snprintf(num, sizeof(num), "%d", w.low);
    int lx = 82 + 32 + 4;
    drawAsciiText(lx, 130, num, INK_BLACK);
    strokeCircle(lx + (int)strlen(num) * 8 + 6, 135, 3, 1, INK_BLACK);
}

static void pageNoNet(const char* title) {
    canvasClear();
    // 顶部照旧显示当前城市：离线/取数失败时也能确认现在选的是哪座
    drawCnText(8, 2, cityCurrent().name, INK_BLACK);
    drawDual(52, 22, ICONS[8].black, ICONS[8].red, 48, 48);   // 断网图标
    drawCnCenter(80, title, INK_BLACK);
    hLine(46, 105, 104, INK_BLACK);
    drawCnCenter(116, "按任意键重试", INK_BLACK);
    if (g_batteryPct >= 0) drawBattery(146, 2, g_batteryPct); // 右上角，和天气页的日期同位置
}

/**
 * 配网页：顶部提示 + 二维码 + 底部 IP。
 * 二维码内容是 WiFi 配置格式（WIFI:T:WPA;S:...;P:...;;），
 * 手机扫码会直接连上设备热点，再由 captive portal 自动弹出配置页。
 */
static bool pageProvisioning(const char* apSsid, const char* apPass) {
    canvasClear();
    drawCnCenter(1, "扫码配网", INK_BLACK);

    const String payload = String("WIFI:T:WPA;S:") + apSsid + ";P:" + apPass + ";;";

    QRCode qr;
    static uint8_t qrBuf[192];   // 版本 3（29x29）实际约 106 字节
    if (qrcode_initText(&qr, qrBuf, 3, 0, payload.c_str()) != 0) {
        logLine("[QR] payload exceeds version 3 capacity");
        return false;
    }

    const int scale = 3;          // 3px/模块，整体 111x111，152 屏放得下且能扫
    const int quiet = 4;          // 静区 4 模块，少了扫不出来
    const int total = (qr.size + quiet * 2) * scale;
    const int ox = (EPD_WIDTH - total) / 2;
    const int oy = 19;

    fillRect(ox, oy, total, total, INK_WHITE);   // 静区必须是白的
    for (uint8_t y = 0; y < qr.size; y++)
        for (uint8_t x = 0; x < qr.size; x++)
            if (qrcode_getModule(&qr, x, y))
                fillRect(ox + (quiet + x) * scale, oy + (quiet + y) * scale,
                         scale, scale, INK_BLACK);

    drawCnCenter(132, "扫码后点加入", INK_BLACK);
    logf("[QR] %dx%d modules, %dpx/module, payload %u bytes\n",
         qr.size, qr.size, scale, (unsigned)payload.length());
    return true;
}

/**
 * 设置页：二维码内容就是网页地址，手机扫码直接打开。
 * 版式：标题 16px + 111px 二维码 + 底部一行（传图入口给 IP，城市入口给当前城市）。
 * forCity 只影响标题和底注，二维码指向同一个网页，页面里两块内容都在。
 */
static bool pageLink(const char* url, bool forCity)
{
    canvasClear();
    drawCnCenter(1, forCity ? "扫码设置城市" : "扫码传图", INK_BLACK);

    QRCode qr;
    static uint8_t qrBuf[192];
    if (qrcode_initText(&qr, qrBuf, 3, 0, url) != 0)
    {
        logLine("[QR] url exceeds version 3 capacity");
        return false;
    }

    const int scale = 3; // 3px/模块，111x111
    const int quiet = 4; // 静区少了扫不出来
    const int total = (qr.size + quiet * 2) * scale;
    const int ox = (EPD_WIDTH - total) / 2;
    const int oy = 19; // 19+111=130，给底部 IP 留 2px 间隙

    fillRect(ox, oy, total, total, INK_WHITE);
    for (uint8_t y = 0; y < qr.size; y++)
        for (uint8_t x = 0; x < qr.size; x++)
            if (qrcode_getModule(&qr, x, y))
                fillRect(ox + (quiet + x) * scale, oy + (quiet + y) * scale,
                         scale, scale, INK_BLACK);

    const String ip = WiFi.localIP().toString();
    if (forCity)
    {
        drawCnCenter(132, cityCurrent().name, INK_BLACK);
    }
    else
    {
        drawAsciiText((EPD_WIDTH - (int)ip.length() * 8) / 2, 132, ip.c_str(), INK_BLACK);
    }
    logf("[QR] %s page %s, %dx%d modules\n", forCity ? "city" : "photo", url, qr.size, qr.size);
    return true;
}

/* ============================================================================
 * 刷新与指示灯
 * ==========================================================================*/
static uint32_t g_epdTicks = 0; // 刷屏等待期间推进了几帧灯（诊断用）

static void onBusyTick(unsigned long /*elapsed_ms*/, bool refreshing) {
    // 等待循环里约每 20ms 调一次。刷屏期间不亮灯（见文件头的指示灯说明），
    // 这里只推进波形、记一下帧数当诊断。
    if (refreshing) g_epdTicks++;
    rgbTick();
}

/**
 * 刷屏：整个过程 15~20 秒。
 * 进屏之前先 rgbOff()，把上一个状态（看图绿 / 配网橙）淡出掉：
 * 刷屏本身就是最明显的进度提示，不需要再用灯重复一遍。
 * 刷完由调用方决定要不要点亮——只有「图已经上屏」和「配网二维码就绪」
 * 两种情况会再亮起来。
 */
static void refreshScreen() {
    logLine("[EPD] refreshing ...");
    rgbOff();
    g_epdTicks = 0;
    const uint32_t t0 = millis();
    g_epd.DisplayFrame(g_black, g_red);
    const uint32_t dt = millis() - t0;
    logf("[EPD] done in %lu ms, %lu led frames\n",
         (unsigned long)dt, (unsigned long)g_epdTicks);
}

/**
 * 进入配网：开热点 -> 画二维码 -> 橙灯闪三下 -> 阻塞等用户保存并连接。
 * 连上 WiFi 后返回 true（不重启设备，避免 S2 在关射频/复位时卡死）。
 */
static bool enterProvisioning() {
    const String apSsid = defaultApSsid();
    const String apPass = AP_PASSWORD;

    startProvisioning(apSsid, apPass);

    if (!pageProvisioning(apSsid.c_str(), apPass.c_str())) {
        pageNoNet("配网失败");
    }
    refreshScreen();

    rgbSetMode(RGB_PROV); // 橙灯闪三下：二维码已就绪（闪完自动熄）

    logf("[AP] open http://%s  or join AP %s\n",
         WiFi.softAPIP().toString().c_str(), apSsid.c_str());
    const bool ok = runProvisioning(); // 连上后返回，不重启
    rgbOff(); // 配网结束，橙色淡出，别把灯带进天气页
    return ok;
}

/* ============================================================================
 * 刷新节奏：每 15 分钟对齐整刻刷新
 * 三色屏全刷约 20 秒且会闪屏，分钟级刷新不现实；对齐到 :00/:15/:30/:45
 * 后，屏上时间正好是整齐的刻钟，也避免固定间隔累积漂移。
 * ==========================================================================*/
static Weather  g_weather;         // 全局天气缓存：刷新失败时保留上一份
#define REFRESH_MINUTES   15
static uint32_t g_nextRefreshMs = 0;
static bool     g_planValid = false;

static uint32_t msToNextQuarter() {
    if (!clockValid()) return REFRESH_MINUTES * 60UL * 1000UL;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    const int into = (tmv.tm_min % REFRESH_MINUTES) * 60 + tmv.tm_sec;
    uint32_t wait = (uint32_t)(REFRESH_MINUTES * 60 - into) * 1000UL;
    return wait < 3000 ? 3000 : wait;      // 贴着整刻时别马上又刷一遍
}

/** 有凭据就重连，没凭据/连不上就进配网；返回 WiFi 是否可用 */
static bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    String ssid, pass;
    if (!loadWifiCreds(ssid, pass)) {
        logLine("[WiFi] no credentials, start provisioning");
        return enterProvisioning();
    }
    return connectWiFi(ssid, pass);
}

/**
 * 屏上现在画的是不是照片。
 * 注意它和 g_mode 不是一回事：传完图后仍停在 UI_LINK（网页继续开着，
 * 手机可以接着传下一张），但屏上已经是照片了。KEY2 的语义取决于「看到什么」，
 * 而不是「后台还开着什么」，所以单独记一个。
 */
static bool g_showingPhoto = false;

/* ============================================================================
 * 低功耗计时：任何按键、任何一次刷屏都算「有操作」
 * ==========================================================================*/
#define IDLE_SLEEP_MS   60000UL

static uint32_t g_lastActiveMs = 0; // 最后一次「有操作」的时刻

/** 摸一下：按键、刷屏都算有操作，重置空闲计时 */
static void touchActivity()
{
    g_lastActiveMs = millis();
}

/** 绘制当前页面并刷屏，然后排下一次定时刷新 */
static void drawAndRefresh() {
    char hhmm[8];
    formatClock(hhmm, sizeof(hhmm));
    logf("[UI] draw page, time %s\n", hhmm);

    // 电池：每次画页前采一次（约 100ms）。放在这里而不是 refreshAll()，
    // 是因为 setup() 开机时直接调本函数，不走 refreshAll——放那儿会漏掉开机那一次。
    int adcMv = 0;
    g_batteryMv = batteryMilliVolts(&adcMv);
    g_batteryPct = batteryPercent(g_batteryMv);
    logf("[BAT] adc %d mV -> %d mV (full %d), %d%%\n",
         adcMv, g_batteryMv, batteryFullMilliVolts(), g_batteryPct);

    if (g_weather.valid) pageWeather(g_weather);
    else                 pageNoNet("无网络");
    refreshScreen();
    g_showingPhoto = false; // 屏上是天气页（或断网页）

    const uint32_t wait = msToNextQuarter();
    g_nextRefreshMs = millis() + wait;
    g_planValid = true;
    logf("[PLAN] next refresh in %lu s\n", (unsigned long)(wait / 1000));
    touchActivity(); // 刚刷完，给 1 分钟操作窗口
}

/**
 * 整屏刷新：联网 -> 对时 -> 拉天气 -> 绘制 -> 刷屏。
 * 调用方：15 分钟定时器、手机改完城市、从看图页退回天气页。
 * forceNet=true 是先断开再重连，目前没有调用方，留给以后的手动重连用。
 * 天气拉取失败保留上一份数据，不会把已有页面变成错误页。
 */
static void refreshAll(bool forceNet) {
    if (forceNet) {
        WiFi.disconnect(true, false);
        delay(200);
    }

    const bool online = ensureWiFi();
    if (online) {
        if (!clockValid()) {
            startClockSync();
            logf("[NTP] sync %s\n", waitClock(6000) ? "ok" : "timeout");
        }
        Weather w = g_weather;
        if (fetchWeather(w)) g_weather = w;
        else
            logLine("[HTTP] fetch failed, keep previous weather");
    } else {
        logLine("[WiFi] offline, refresh local clock only");
    }

    drawAndRefresh();
}

/* ============================================================================
 * 看图 / 传图 / 设城市
 * ----------------------------------------------------------------------------
 * 三个界面，靠 g_mode 区分：
 *   UI_WEATHER  天气页，15 分钟定时刷新
 *   UI_LINK     设置页：起网页 + 屏上二维码，手机可以选城市，也可以传图
 *   UI_PHOTO    重放页：把 Flash 里存的上一次传的图刷上去，不联网
 * 后两个界面都暂停定时刷新（照片要一直留着）。退出：KEY1 再长按，
 * 或者 KEY2 长按（屏上是照片时它就是「回天气」，不做无谓重刷）。
 * 手机选完城市会 POST 回来，这里等响应发完（约 1.2 秒）再回天气页刷新。
 * ==========================================================================*/
enum UiMode : uint8_t { UI_WEATHER = 0, UI_LINK, UI_PHOTO };

static UiMode g_mode = UI_WEATHER;

/** 打开设置页：连 WiFi -> 起网页 -> 屏上二维码。forCity 只决定标题与页面落点 */
static void enterLinkMode(bool forCity)
{
    logf("[UI] open settings page: %s\n", forCity ? "city" : "photo");
    g_showingPhoto = false; // 待会画的是二维码页（连不上就是断网页）

    if (!ensureWiFi())
    {
        logLine("[LINK] no network, cannot open settings page");
        pageNoNet("无网络");
        refreshScreen();
        g_mode = UI_WEATHER; // 失败就当没进去，下次再按
        return;
    }

    if (!photoServerStart())
    {
        logLine("[LINK] web server failed to start");
        pageNoNet("无网络");
        refreshScreen();
        g_mode = UI_WEATHER;
        return;
    }

    const String url = photoServerUrl(forCity);
    if (!pageLink(url.c_str(), forCity)) pageNoNet("无网络");
    refreshScreen(); // 刷完不点灯：二维码页不用颜色提示

    g_mode = UI_LINK;
    g_planValid = false; // 暂停 15 分钟定时刷新
    logf("[LINK] scan or open %s  (long-press KEY1 to exit)\n", url.c_str());
}

/** 长按 KEY2：把 Flash 里的上一张刷上去；没有就退回设置页 */
static void showLastPhoto()
{
    logLine("[UI] KEY2 long press: replay last photo");
    if (!photoStoreLoad(g_black, g_red))
    {
        logLine("[PHOTO] no stored photo, open upload page instead");
        if (g_mode != UI_LINK) enterLinkMode(false);
        return;
    }

    if (g_mode == UI_LINK) photoServerStop(); // 看图不必开着网页
    refreshScreen();
    g_showingPhoto = true;
    g_mode = UI_PHOTO;
    g_planValid = false;
    touchActivity(); // 刚上屏，给 1 分钟操作窗口
    rgbSetMode(RGB_OK); // 绿灯闪三下：图已经上屏（闪完自动熄）
    logLine("[PHOTO] last photo shown (long-press KEY2 to exit)");
}

/** 长按「进来的那个键」：停掉网页，回天气页并重排定时刷新 */
static void exitToWeather()
{
    logLine("[UI] long press: back to weather page");
    if (g_mode == UI_LINK) photoServerStop();
    g_mode = UI_WEATHER;
    refreshAll(false); // 回天气页，并重排下一次定时刷新
}

/* ============================================================================
 * 按键：短按开设置页选城市 / 长按进出传图
 * 消抖 + 长短按识别在这里，动作在 handleButtons() 里派发
 * ==========================================================================*/
enum BtnEvent { BTN_NONE = 0, BTN_SHORT, BTN_LONG };

struct Button {
    uint8_t  pin;
    bool     stable;
    bool     raw;
    uint32_t lastChange;
    uint32_t pressStart;
    bool     longSent;
};

static Button g_btnA = {PIN_BTN_A, true, true, 0, 0, false};
static Button g_btnB = {PIN_BTN_B, true, true, 0, 0, false};

static BtnEvent pollButton(Button& b) {
    const bool raw = digitalRead(b.pin);        // 低 = 按下
    const uint32_t now = millis();

    if (raw != b.raw) { b.raw = raw; b.lastChange = now; return BTN_NONE; }
    if (now - b.lastChange < DEBOUNCE_MS) return BTN_NONE;

    if (raw != b.stable) {
        b.stable = raw;
        if (raw == LOW) {
            b.pressStart = now;
            b.longSent = false;
        } else {
            const bool wasShort = (b.pressStart != 0) && !b.longSent;
            b.pressStart = 0;
            if (wasShort) return BTN_SHORT;
        }
        return BTN_NONE;
    }

    if (b.stable == LOW && !b.longSent && b.pressStart != 0 &&
        now - b.pressStart >= LONG_PRESS_MS) {
        b.longSent = true;
        return BTN_LONG;
    }
    return BTN_NONE;
}

/**
 * 按键分工：
 *   短按      = 开设置页选城市（天气页才响应；设置页/看图页里短按不做事）
 *   KEY1 长按 = 扫码传图（已在设置页时 = 退出回天气）
 *   KEY2 长按 = 屏上正是照片时回天气，否则调出上一张（没存过图就落到扫码页）
 */
static void handleButtons() {
    const BtnEvent evA = pollButton(g_btnA);
    const BtnEvent evB = pollButton(g_btnB);

    if ((evA == BTN_SHORT || evB == BTN_SHORT) && g_mode == UI_WEATHER)
    {
        enterLinkMode(true);
    }

    if (evA == BTN_LONG)
    {
        if (g_mode == UI_LINK) exitToWeather();
        else enterLinkMode(false);
    }

    if (evB == BTN_LONG)
    {
        // 正在看图（含「刚传完还留在设置页」那种）就直接回天气，
        // 不然会把同一张图原样重刷一遍，白等 20 秒
        if (g_showingPhoto) exitToWeather();
        else showLastPhoto();
    }

    if (evA != BTN_NONE || evB != BTN_NONE) touchActivity();
}

/* ============================================================================
 * 低功耗：空闲 1 分钟进深睡
 * ----------------------------------------------------------------------------
 *   天气页：定时唤醒（对齐下一个整刻）+ 按键唤醒，醒来重新联网刷天气
 *   看图页：只等按键唤醒 —— 照片是墨水屏，断电也留着，正好当相框
 *   设置页：不睡 —— 网页服务一睡就断，手机那边会莫名打不开
 * 深睡丢 RAM，所以每次醒来都是一次完整重启：setup 重跑、屏幕重刷。
 * 代价是每次刷新多花 3~5 秒重连 WiFi + 对时，换来其余时间 20µA 级的待机。
 * ==========================================================================*/
/** 进入深睡，不返回 */
static void enterDeepSleep()
{
    // 有人正按着键就先别睡：否则一躺下就被这个按键立刻叫醒
    if (digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW)
    {
        touchActivity();
        return;
    }

    const bool photoMode = (g_mode == UI_PHOTO);

    uint32_t waitMs = 0;
    if (!photoMode)
    {
        waitMs = msToNextQuarter();
        if (waitMs < 60000UL) waitMs = 60000UL; // 别刚躺下又爬起来
    }

    logf("[PWR] idle %lu s, deep sleep (%s)\n",
         (unsigned long)((millis() - g_lastActiveMs) / 1000),
         photoMode ? "key wake only" : "key or timer wake");
    if (!photoMode)
        logf("[PWR] timer wake in %lu s\n", (unsigned long)(waitMs / 1000));

    if (g_mode == UI_LINK) photoServerStop(); // 正常进不来，保险
    g_epd.Sleep(); // 面板断电，画面留着
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // 两个按键都配成 RTC 上拉，任意一个拉低就醒。
    // RTC 外设域要单独保持供电，否则内部上拉跟着掉电，引脚浮空会乱醒。
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    for (int pin : {PIN_BTN_A, PIN_BTN_B})
    {
        rtc_gpio_pulldown_dis((gpio_num_t)pin);
        rtc_gpio_pullup_en((gpio_num_t)pin);
    }
    const uint64_t mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B);
    esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (!photoMode) esp_sleep_enable_timer_wakeup((uint64_t)waitMs * 1000ULL);

    // 指示灯数据脚深睡时是悬空的，可能被干扰出随机颜色；吊住低电平
    gpio_hold_en((gpio_num_t)BOARD_RGB_PIN);
    gpio_deep_sleep_hold_en();

    delay(60); // 让串口把上面几行吐完
    esp_deep_sleep_start();
}

/* ============================================================================
 * 主流程
 * ==========================================================================*/
void setup() {
    logBegin(); // release 构建（NETEINK_LOG=0）下是空语句
#if NETEINK_LOG
    delay(800); // 留时间给人打开串口监视器；release 不需要
#endif
    rgbBegin(BOARD_RGB_PIN, BOARD_RGB_PEAK); // 指示灯引擎（上电先清灯）
    batteryBegin(); // BAT_ADC = GPIO3

    // 解除深睡时对指示灯引脚的吊住（见 enterDeepSleep）
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)BOARD_RGB_PIN);

    // 深睡前把按键配成了 RTC 上拉，醒来先还回普通 GPIO，否则 digitalRead 读不到
    rtc_gpio_deinit((gpio_num_t)PIN_BTN_A);
    rtc_gpio_deinit((gpio_num_t)PIN_BTN_B);
    pinMode(PIN_BTN_A, INPUT_PULLUP);
    pinMode(PIN_BTN_B, INPUT_PULLUP);

    const esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    if (wake == ESP_SLEEP_WAKEUP_TIMER)
        logLine("[PWR] wake by timer");
    else if (wake == ESP_SLEEP_WAKEUP_EXT1)
        logLine("[PWR] wake by key");
    else
        logLine("[PWR] cold boot");

    cityBegin(); // 恢复上次选的城市（NVS），没存过就是上海

    logLine("\n========================================");
    logLine("  NetEink / clock + weather");
    logf("  city code: %lu (short press to change)\n", (unsigned long)cityCurrent().code);
    logLine("========================================");

    g_epd.SetBusyCallback(onBusyTick);
    logLine("[1/5] init display ...");
    if (g_epd.Init() != 0) {
        logLine("[EPD] init failed: check SPI wiring / power");
        return;
    }

    // 2) 有凭据就联网；没配过网或连不上 -> 进配网（内部阻塞，连上后返回）
    String ssid, pass;
    bool online = false;
    if (loadWifiCreds(ssid, pass)) {
        logf("[2/5] connect WiFi %s ...\n", ssid.c_str());
        online = connectWiFi(ssid, pass);
    } else {
        logLine("[2/5] not provisioned yet");
    }

    if (!online) {
        logLine("[2/5] enter provisioning");
        online = enterProvisioning();   // 配网成功后 WiFi 已连上
    }

    // 3) 对时：同步后 RTC 会自己走，之后每 15 分钟再校一次
    if (online) {
        logLine("[3/5] NTP sync ...");
        startClockSync();
        if (waitClock(6000)) {
            char hhmm[8], date[8];
            formatClock(hhmm, sizeof(hhmm));
            formatDate(date, sizeof(date));
            logf("[NTP] now %s %s\n", date, hhmm);
        } else {
            logLine("[NTP] not synced, clock shows --:--");
        }
    } else {
        logLine("[3/5] offline, skip NTP");
    }

    // 4) 天气：失败不影响时钟，页面会退化成「无网络」
    logLine("[4/5] fetch weather ...");
    if (online) {
        Weather w = g_weather;
        if (fetchWeather(w)) g_weather = w;
        else
            logLine("[HTTP] weather fetch failed");
    }

    // 5) 绘制并刷新；drawAndRefresh 内部会排好下一次 15 分钟刷新
    logLine("[5/5] draw and refresh ...");
    drawAndRefresh();

    logLine("[TIP] short press = set city, long KEY1 = photo, long KEY2 = last photo");
}

void loop() {
    // 指示灯：主循环持续推进，闪三次的倒计时在 rgbTick 里走
    rgbTick();

    // 按键：短按选城市 / 长按 KEY1 扫码 / 长按 KEY2 看上一张
    handleButtons();

    // 设置页：先伺服手机请求，再检查有没有新图或新城
    if (g_mode == UI_LINK)
    {
        photoServerLoop();
        if (photoTakeFrame(g_black, g_red))
        {
            logf("[PHOTO] showing photo #%u\n", (unsigned)photoFrameCount());
            // 先存一份再刷屏：万一刷到一半断电，KEY2 还能把这张调出来
            photoStoreSave(g_black, g_red);
            refreshScreen();
            g_showingPhoto = true; // 屏上已经是照片，KEY2 该回天气而不是重刷
            photoMarkDone();
            rgbSetMode(RGB_OK); // 绿灯闪三下：这张已经上屏
        }
        // 手机选完城市（或用了定位）：等响应发完，停网页、回天气页刷新城
        if (photoCityChangeDue())
        {
            logLine("[LINK] city changed from phone, refresh weather page");
            photoServerStop();
            g_mode = UI_WEATHER;
            g_weather.valid = false; // 作废旧城数据，别把上海的温度挂到新城名下
            refreshAll(false);
        }
    }

    // 到点刷新：每 15 分钟，对齐 :00/:15/:30/:45（看图/传图时暂停）
    if (g_planValid && (int32_t)(millis() - g_nextRefreshMs) >= 0) {
        logLine("[UI] scheduled: 15-minute refresh");
        refreshAll(false);
    }

    // 空闲超时 -> 深睡。设置页不睡（网页服务一睡就断）
    if (g_mode != UI_LINK && (uint32_t)(millis() - g_lastActiveMs) >= IDLE_SLEEP_MS)
    {
        enterDeepSleep();
    }

    delay(5);
}
