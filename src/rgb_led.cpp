/**
 * 呼吸灯引擎实现，接口说明见 rgb_led.h
 * ==========================================================================*/
#include "rgb_led.h"

#include <Arduino.h>
#include <math.h>

#include "esp32-hal-rgb-led.h"

namespace {

constexpr uint32_t kTickMs = 20;    // 波形推进周期，20ms ≈ 50fps，肉眼连续
constexpr float    kEase   = 0.25f; // 输出缓动系数（每 tick 走 1/4 剩余量）

struct Preset {
    uint8_t  r, g, b;      // 基色
    uint8_t  levelPct;     // 相对峰值百分比：绿光视觉更亮，所以压低一些
    uint16_t period;       // 呼吸周期 ms
    uint8_t  floorPct;     // 最暗时保留的百分比，避免完全熄灭像在闪
};

// 下标与 RgbMode 一一对应
const Preset kPresets[] = {
    {  0,   0,   0,   0,    0,  0},   // RGB_OFF
    {  0,  70, 255, 100, 2600,  5},   // RGB_WAIT    蓝
    {255,  30,   0,  95, 1500,  8},   // RGB_WORK    红
    {  0, 255,  70,  66, 2800,  5},   // RGB_CONFIG  绿
    {  0, 255,  70,  66, 2000, 45},   // RGB_OK      绿（浅呼吸，接近常亮）
};

uint8_t  g_pin      = 9;
uint8_t  g_peak     = 96;
RgbMode  g_mode     = RGB_OFF;
uint32_t g_modeAt   = 0;      // 本状态的起始时刻：相位从这里重新计时
uint32_t g_lastTick = 0;
float    g_out[3]   = {0.0f, 0.0f, 0.0f};   // 平滑后的实际输出
uint8_t  g_sent[3]  = {0, 0, 0};            // 上次写入总线的值
bool     g_sentOk   = false;

void writeRgb(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWriteOrdered(g_pin, LED_COLOR_ORDER_GRB, r, g, b);
    g_sent[0] = r;
    g_sent[1] = g;
    g_sent[2] = b;
    g_sentOk = true;
}

}  // namespace

void rgbBegin(uint8_t pin, uint8_t peak) {
    g_pin  = pin;
    g_peak = peak;
    g_mode = RGB_OFF;
    g_out[0] = g_out[1] = g_out[2] = 0.0f;
    writeRgb(0, 0, 0);        // 上电先清灯：WS2812 会保留上一次的颜色
}

void rgbSetMode(RgbMode mode) {
    if (mode == RGB_OFF) {
        rgbOff();
        return;
    }
    if (mode == g_mode) return;   // 同状态不重置相位，呼吸不会被反复打断
    g_mode   = mode;
    g_modeAt = millis();          // 从最暗处淡入，避免切换瞬间跳亮
}

void rgbOff() {
    if (g_mode == RGB_OFF) return;
    // 不立刻写 0：把目标亮度交给 rgbTick() 缓动下来，避免「啪」地熄灭。
    // 每 tick 掉 1/4，约 0.38s 从最亮淡到全黑，观感是「熄灭」而不是「断电」。
    g_mode = RGB_OFF;
}

void rgbTick() {
    const uint32_t now = millis();
    if (now - g_lastTick < kTickMs) return;
    g_lastTick = now;

    float target[3] = {0.0f, 0.0f, 0.0f};

    if (g_mode != RGB_OFF) {
        const Preset& p = kPresets[g_mode];

        // 相位 0..1，升余弦 0->1->0；两端导数为 0，所以不会出现折点
        const float phase = (float)((now - g_modeAt) % p.period) / (float)p.period;
        float amp = 0.5f - 0.5f * cosf(phase * 2.0f * (float)PI);

        // 留一点底光：最暗也有 floor%，观感是「变暗」而不是「关灯」
        const float lo = (float)p.floorPct * 0.01f;
        amp = lo + (1.0f - lo) * amp;

        const float k = amp * ((float)g_peak / 255.0f) * ((float)p.levelPct * 0.01f);
        target[0] = (float)p.r * k;
        target[1] = (float)p.g * k;
        target[2] = (float)p.b * k;
    }

    // 朝目标缓动：既让呼吸曲线保持光滑，也让「切换颜色/熄灭」变成渐变
    uint8_t out[3];
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        g_out[i] += (target[i] - g_out[i]) * kEase;
        int v = (int)(g_out[i] + 0.5f);
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        out[i] = (uint8_t)v;
        if (!g_sentOk || out[i] != g_sent[i]) changed = true;
    }

    if (changed) writeRgb(out[0], out[1], out[2]);
}
