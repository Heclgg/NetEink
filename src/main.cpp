/**
 *  墨水屏天气牌（时钟 + 上海天气）—— 自画 ESP32-S2 板
 * ----------------------------------------------------------------------------
 *  流程：
 *    上电 -> 连 WiFi -> NTP 对时 -> 拉上海天气
 *         -> 绘制 152x152 三色界面 -> 刷新
 *         -> 之后每 15 分钟（对齐 :00/:15/:30/:45）对时 + 拉天气 + 刷屏
 *      |             |
 *      |             +-- 失败：断网图标 + "无网络"，时间显示 --:--
 *      +-- 成功：顶部城市/日期/时间，左侧天气图标，右侧大号温度，底部高低温
 *
 *  按键（KEY1/GPIO1、KEY2/GPIO2，任意一个）：
 *    短按 -> 立即刷新（断网会自动重连；天气失败保留上一份数据）
 *    长按 1.5s -> 强制重连 WiFi 后刷新
 *
 *  指示灯：板载 WS2812(GPIO9)，连续呼吸（见 rgb_led.h），不是硬开关闪烁
 *    蓝色慢呼吸 = 联网/对时/取天气   红色呼吸 = 刷屏中
 *    绿色慢呼吸 = 配网等待扫码       绿色浅呼吸 2.5s = 本轮刷新完成
 *
 *  屏幕：微雪 1.54inch e-Paper (C)，152x152，黑/白/红三色
 *  素材：src/ui_assets.h 由 tools/gen_ui_assets.py 生成（改字/改图重跑脚本）
 *        图标/图片在生成端已做 4x 超采样 + 白/黑/红三色误差扩散抖动，
 *        用点阵疏密模拟中间调，边缘/渐变更顺滑；落到屏上仍是 1-bit 双层
 *        位图，MCU 端没有灰度可还原，所以这里只直出、不再抖动。
 *        字模保持硬阈值二值化，避免笔画被抖动打毛。
 *  日志：USB CDC(COM9) 与 UART0(GPIO43/44) 双路输出
 */
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "epd1in54c.h"
#include "ui_assets.h"
#include "qrcode.h"
#include "provisioning.h"
#include "rgb_led.h"

/* ============================================================================
 * 配置
 * WiFi 凭据不再写死在代码里：首次上电（或连不上）会开热点进入配网，
 * 凭据保存在 NVS（namespace: wifi），由 provisioning.cpp 管理。
 * ==========================================================================*/

// 上海 city code = 101020100；这个接口免 key、走 HTTP，S2 上没有 TLS 内存压力
#define WEATHER_URL            "http://t.weather.itboy.net/api/weather/city/101020100"

#define AP_PASSWORD            "12345678"   // 配网热点密码（WPA2 至少 8 位）

#define WIFI_TIMEOUT_MS        15000
#define HTTP_TIMEOUT_MS         8000
#define DEBOUNCE_MS               30
#define LONG_PRESS_MS           1500

#ifndef BOARD_RGB_PIN
#define BOARD_RGB_PIN              9
#endif
#ifndef BOARD_RGB_PEAK
#define BOARD_RGB_PEAK            96    // 呼吸峰值亮度 0-255：嫌亮就调小
#endif

#define PIN_BTN_A                  1   // KEY1
#define PIN_BTN_B                  2   // KEY2

/* ============================================================================
 * 双路日志：USB CDC(COM9) + UART0(GPIO43/44)
 * ==========================================================================*/
static void logf(const char* fmt, ...) {
    char buf[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
    Serial0.print(buf);
}

static void logLine(const char* s = "") {
    Serial.println(s);
    Serial0.println(s);
}

/* ============================================================================
 * 板载 WS2812（GPIO9）：呼吸波形在 rgb_led.cpp，这里只决定「什么状态什么颜色」
 * ==========================================================================*/
#define LED_CONFIRM_MS  2500        // 刷新完成后绿色浅呼吸的保持时间
static uint32_t g_ledOffAt = 0;     // 到点自动熄灭；0 = 没有待处理的熄灭

static void ledConfirm(uint32_t holdMs = LED_CONFIRM_MS) {
    rgbSetMode(RGB_OK);
    g_ledOffAt = millis() + holdMs;
}

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
        rgbTick();                 // 对时要等好几秒，呼吸灯不能停
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
 * 天气数据
 * ==========================================================================*/
struct Weather {
    bool valid = false;
    int  temp  = 0;
    int  high  = 0;
    int  low   = 0;
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

static bool connectWiFi(const String& ssid, const String& pass) {
    logf("[WiFi] 连接 %s ...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    rgbSetMode(RGB_WAIT);              // 蓝色慢呼吸，替代原来的 250ms 硬闪
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        rgbTick();
        delay(10);
    }

    if (WiFi.status() == WL_CONNECTED) {
        logf("[WiFi] 已连接 %s  RSSI=%d\n",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }
    rgbOff();
    logLine("[WiFi] 连接失败");
    return false;
}

static bool fetchWeather(Weather& w) {
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(WEATHER_URL)) {
        logLine("[HTTP] begin 失败");
        return false;
    }

    const int code = http.GET();
    if (code != 200) {
        logf("[HTTP] 状态码 %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    logf("[HTTP] 收到 %u 字节\n", (unsigned)body.length());
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
        logf("[HTTP] 天气 %s\n", w.type);
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
    logf("[HTTP] %s %d度 高%d 低%d %s\n", w.date, w.temp, w.high, w.low, w.type);
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
static void pageWeather(const Weather& w) {
    canvasClear();

    // 顶部：城市 + 日期（日期取本地时间，与时钟同源，不依赖天气接口）
    drawCnText(8, 2, "上海", INK_BLACK);
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
    drawDual(52, 22, ICONS[8].black, ICONS[8].red, 48, 48);   // 断网图标
    drawCnCenter(80, title, INK_BLACK);
    hLine(46, 105, 104, INK_BLACK);
    drawCnCenter(116, "按任意键重试", INK_BLACK);
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
        logLine("[QR] 内容超出版本 3 容量");
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
    logf("[QR] %dx%d 模块，%dpx/模块，负载 %u 字节\n",
         qr.size, qr.size, scale, (unsigned)payload.length());
    return true;
}

/* ============================================================================
 * 刷新与指示灯
 * ==========================================================================*/
static void onBusyTick(unsigned long /*elapsed_ms*/, bool refreshing) {
    // 等待循环里约每 20ms 调一次：只切状态 + 推进波形，亮度由引擎自己算
    rgbSetMode(refreshing ? RGB_WORK : RGB_WAIT);
    rgbTick();
}

static void refreshScreen() {
    logLine("[EPD] 刷新中 ...");
    const uint32_t t0 = millis();
    g_epd.DisplayFrame(g_black, g_red);
    logf("[EPD] 完成，耗时 %lu ms\n", (unsigned long)(millis() - t0));
}

/**
 * 进入配网：开热点 -> 画二维码 -> 绿灯慢呼吸 -> 阻塞等用户保存并连接。
 * 连上 WiFi 后返回 true（不重启设备，避免 S2 在关射频/复位时卡死）。
 */
static bool enterProvisioning() {
    g_ledOffAt = 0;
    const String apSsid = defaultApSsid();
    const String apPass = AP_PASSWORD;

    startProvisioning(apSsid, apPass);

    if (!pageProvisioning(apSsid.c_str(), apPass.c_str())) {
        pageNoNet("配网失败");
    }
    refreshScreen();

    rgbSetMode(RGB_CONFIG);          // 绿色慢呼吸：等待手机提交

    logf("[AP] 扫码或访问 http://%s ，热点 %s\n",
         WiFi.softAPIP().toString().c_str(), apSsid.c_str());
    return runProvisioning();        // 连上后返回，不重启
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
        logLine("[WiFi] 无凭据，进入配网");
        return enterProvisioning();
    }
    return connectWiFi(ssid, pass);
}

/** 绘制当前页面并刷屏，然后排下一次定时刷新 */
static void drawAndRefresh() {
    char hhmm[8];
    formatClock(hhmm, sizeof(hhmm));
    logf("[UI] 绘制页面，时间 %s\n", hhmm);

    if (g_weather.valid) pageWeather(g_weather);
    else                 pageNoNet("无网络");
    refreshScreen();

    // 刷完给个「完成」提示：绿灯浅呼吸 2.5s，由 loop() 到点熄灭
    if (g_weather.valid) ledConfirm();
    else                 rgbOff();

    const uint32_t wait = msToNextQuarter();
    g_nextRefreshMs = millis() + wait;
    g_planValid = true;
    logf("[PLAN] 下次刷新 %lu 秒后\n", (unsigned long)(wait / 1000));
}

/**
 * 整屏刷新：联网 -> 对时 -> 拉天气 -> 绘制 -> 刷屏。
 * forceNet=true（长按）先断开再重连；否则只在掉线时重连。
 * 天气拉取失败保留上一份数据，不会把已有页面变成错误页。
 */
static void refreshAll(bool forceNet) {
    g_ledOffAt = 0;                  // 取消上一次「完成」提示的待熄灭
    if (forceNet) {
        WiFi.disconnect(true, false);
        delay(200);
    }

    const bool online = ensureWiFi();
    if (online) {
        rgbSetMode(RGB_WAIT);        // 对时 / 取天气期间蓝色慢呼吸
        if (!clockValid()) {
            startClockSync();
            logf("[NTP] 对时%s\n", waitClock(6000) ? "成功" : "超时");
        }
        Weather w = g_weather;
        if (fetchWeather(w)) g_weather = w;
        else                 logLine("[HTTP] 本次未取到天气，保留上一份");
    } else {
        logLine("[WiFi] 离线，只刷新本地时间");
    }

    drawAndRefresh();
}

/* ============================================================================
 * 按键
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

/* ============================================================================
 * 主流程
 * ==========================================================================*/
void setup() {
    Serial.begin(115200);
    Serial0.begin(115200);
    delay(800);
    rgbBegin(BOARD_RGB_PIN, BOARD_RGB_PEAK);   // 呼吸灯引擎（上电先清灯）

    pinMode(PIN_BTN_A, INPUT_PULLUP);
    pinMode(PIN_BTN_B, INPUT_PULLUP);

    logLine("\n========================================");
    logLine("  NetEink / 时钟 + 上海天气");
    logLine("========================================");

    g_epd.SetBusyCallback(onBusyTick);
    logLine("[1/5] 初始化屏幕 ...");
    if (g_epd.Init() != 0) {
        logLine("[EPD] 初始化失败：检查 SPI 接线 / 供电");
        return;
    }

    // 2) 有凭据就联网；没配过网或连不上 -> 进配网（内部阻塞，连上后返回）
    String ssid, pass;
    bool online = false;
    if (loadWifiCreds(ssid, pass)) {
        logf("[2/5] 连接 WiFi %s ...\n", ssid.c_str());
        online = connectWiFi(ssid, pass);
    } else {
        logLine("[2/5] 尚未配网");
    }

    if (!online) {
        logLine("[2/5] 进入配网模式");
        online = enterProvisioning();   // 配网成功后 WiFi 已连上
    }

    // 3) 对时：同步后 RTC 会自己走，之后每 15 分钟再校一次
    if (online) {
        logLine("[3/5] NTP 对时 ...");
        startClockSync();
        if (waitClock(6000)) {
            char hhmm[8], date[8];
            formatClock(hhmm, sizeof(hhmm));
            formatDate(date, sizeof(date));
            logf("[NTP] 当前 %s %s\n", date, hhmm);
        } else {
            logLine("[NTP] 未同步，时间显示 --:--");
        }
    } else {
        logLine("[3/5] 离线，跳过对时");
    }

    // 4) 天气：失败不影响时钟，页面会退化成「无网络」
    logLine("[4/5] 获取天气 ...");
    if (online) {
        Weather w = g_weather;
        if (fetchWeather(w)) g_weather = w;
        else                 logLine("[HTTP] 天气获取失败");
    }

    // 5) 绘制并刷新；drawAndRefresh 内部会排好下一次 15 分钟刷新
    logLine("[5/5] 绘制并刷新 ...");
    drawAndRefresh();

    logLine("[提示] 短按立即刷新，长按强制重连");
}

void loop() {
    // 呼吸灯：主循环持续推进；「刷新完成」的绿灯到点自动熄灭
    if (g_ledOffAt && (int32_t)(millis() - g_ledOffAt) >= 0) {
        g_ledOffAt = 0;
        rgbOff();
    }
    rgbTick();

    // 任意一个按键：短按 -> 立即刷新；长按 -> 强制重连 WiFi 后刷新
    const BtnEvent evA = pollButton(g_btnA);
    const BtnEvent evB = pollButton(g_btnB);
    const BtnEvent ev  = (evA != BTN_NONE) ? evA : evB;

    if (ev == BTN_SHORT) {
        logLine("[UI] 短按：立即刷新");
        refreshAll(false);
    } else if (ev == BTN_LONG) {
        logLine("[UI] 长按：强制重连并刷新");
        refreshAll(true);
    }

    // 到点刷新：每 15 分钟，对齐 :00/:15/:30/:45
    if (g_planValid && (int32_t)(millis() - g_nextRefreshMs) >= 0) {
        logLine("[UI] 到点：15 分钟定时刷新");
        refreshAll(false);
    }

    delay(5);
}
