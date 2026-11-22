/**
 * 呼吸灯引擎实现，接口说明见 rgb_led.h
 * ==========================================================================*/
#include "rgb_led.h"

#include <Arduino.h>

#if NETEINK_LED

#include <math.h>

#include "esp32-hal-rmt.h"
#include "log.h"

namespace {

constexpr uint32_t kTickMs = 20;    // 波形推进周期，20ms ≈ 50fps，肉眼连续
constexpr float    kEase   = 0.25f; // 输出缓动系数（每 tick 走 1/4 剩余量）

struct Preset {
    const char* name; // 日志里用，方便确认跑的是哪版固件
    uint8_t  r, g, b;      // 基色
    uint8_t  levelPct;     // 相对峰值百分比：绿光视觉更亮，所以压低一些
    uint16_t period; // 一次「亮-暗」的周期 ms
    uint8_t floorPct; // 最暗时保留的百分比
    uint8_t times; // 闪几次后自动熄灭；0 = 一直亮着（现在不用）
};

// 下标与 RgbMode 一一对应
//
// 这两个都是「提示一下」而不是「持续状态」，所以参数和以前的常亮呼吸不同：
//   1) period 取 700ms：一次亮暗 0.7 秒，连起来是「闪三下」，总共约 2.1 秒。
//      之前常亮时用 3.5s 是为了像「吸气—呼气」，拿来做三次闪就变成十秒了。
//   2) floorPct 取 10%：两下之间要真的暗下去，否则三条亮纹糊成一片。
//   3) times = 3：闪完立刻写黑，不依赖后面还有没有人调 rgbTick。
const Preset kPresets[] = {
    {"off", 0, 0, 0, 0, 0, 0, 0}, // RGB_OFF
    {"green", 0, 255, 70, 70, 700, 10, 3}, // RGB_OK    绿：图片刚上屏，闪三下
    {"orange", 255, 120, 0, 75, 700, 10, 3}, // RGB_PROV  橙：配网二维码刚就绪，闪三下
};

uint8_t  g_pin      = 9;
uint8_t  g_peak     = 96;
RgbMode  g_mode     = RGB_OFF;
uint32_t g_modeAt   = 0;      // 本状态的起始时刻：相位从这里重新计时
uint32_t g_lastTick = 0;
float    g_out[3]   = {0.0f, 0.0f, 0.0f};   // 平滑后的实际输出
uint8_t g_sent[3] = {0, 0, 0}; // 上次写入总线的值（失败时不更新）
bool     g_sentOk   = false;
bool g_clearAgain = false; // 熄灭后再补一帧黑（防单帧被打断）
bool g_warned = false; // 写失败只喊一次，别刷屏
bool g_busStale = false; // 上一帧没等到 TX_DONE：总线卡住，要重建

/**
 * 自己拼 24 个 RMT 符号发给 WS2812，不用 rgbLedWriteOrdered()，原因有三：
 *   1) 那个函数返回 void：写没写进去我们不知道；
 *   2) 它内部用 RMT_WAIT_FOR_EVER，等不到 TX_DONE 就永久卡住；
 *   3) 更要命的是 HAL 的行为：_rmtWrite() 每次发送前先清 TX_DONE 标志，
 *      只有发送完成的 ISR 才置回来。只要有一次 rmt_transmit 失败或 ISR 漏掉，
 *      标志就永远是 0，之后每次写都在入口直接 return false（不发送），
 *      灯就冻在最后一帧——「闪完还常亮」正是这个症状。
 *      所以这里失败后会把总线拆了重建（rmtInit 会把标志重新置位）。
 * 时序：RMT tick = 0.1us（rmtInit 传 10MHz），WS2812 的 0 = 0.4/0.8us，1 = 0.8/0.4us。
 */
bool sendFrame(uint8_t r, uint8_t g, uint8_t b)
{
    if (g_busStale)
    {
        rmtDeinit(g_pin);
        if (!rmtInit(g_pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000))
        {
            if (!g_warned)
            {
                g_warned = true;
                logLine("[LED] RMT re-init failed, LED disabled");
            }
            return false;
        }
        g_busStale = false;
        if (!g_warned)
        {
            g_warned = true; // 只喊一次，真好了会由成功路径清掉
            logLine("[LED] RMT bus reset after a failed frame");
        }
    }

    rmt_data_t sym[24];
    const uint8_t grb[3] = {g, r, b};
    int i = 0;
    for (int c = 0; c < 3; c++)
    {
        for (int bit = 7; bit >= 0; bit--)
        {
            const bool one = (grb[c] >> bit) & 1;
            sym[i].level0 = 1;
            sym[i].duration0 = one ? 8 : 4;
            sym[i].level1 = 0;
            sym[i].duration1 = one ? 4 : 8;
            i++;
        }
    }
    if (rmtWrite(g_pin, sym, RMT_SYMBOLS_OF(sym), 20)) return true;

    g_busStale = true; // 下次进来先复位总线
    return false;
}

bool writeRgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!sendFrame(r, g, b)) return false; // 没发出去：g_sent 保持不变，下一 tick 重试
    g_sent[0] = r;
    g_sent[1] = g;
    g_sent[2] = b;
    g_sentOk = true;
    return true;
}

}  // namespace

void rgbBegin(uint8_t pin, uint8_t peak) {
    g_pin  = pin;
    g_peak = peak;
    g_mode = RGB_OFF;
    g_out[0] = g_out[1] = g_out[2] = 0.0f;
    g_clearAgain = false;
    g_warned = false;
    g_busStale = false;

    // RMT 通道只建一次：10MHz -> 1 tick = 0.1us，正好拼 WS2812 的 0.4/0.8us
    if (!rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000))
    {
        logLine("[LED] RMT init failed, LED disabled");
        return;
    }

    const bool ok = writeRgb(0, 0, 0); // 上电先清灯：WS2812 会保留上一次的颜色
    // 上电打一条，用来确认板子里跑的是这一版（旧版是常亮呼吸，没有这条日志）
    logf("[LED] ready on pin %u, %s x%d / %s x%d, %u ms each, clear %s\n",
         g_pin,
         kPresets[RGB_OK].name, kPresets[RGB_OK].times,
         kPresets[RGB_PROV].name, kPresets[RGB_PROV].times,
         kPresets[RGB_OK].period, ok ? "ok" : "FAILED");
}

void rgbSetMode(RgbMode mode) {
    if (mode == RGB_OFF) {
        rgbOff();
        return;
    }
    if (mode == g_mode) return; // 同状态不重置相位，闪到一半不会被反复打断
    g_mode   = mode;
    g_modeAt = millis();          // 从最暗处淡入，避免切换瞬间跳亮
    logf("[LED] %s x%d, period %u ms\n",
         kPresets[mode].name, kPresets[mode].times, kPresets[mode].period);
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

    // 上一帧刚熄灭：补发一次黑。WS2812 靠单帧锁存，多打一帧不花钱，
    // 但能兜住「最后一帧被复位/干扰吃掉」导致灯常亮的情况。
    if (g_clearAgain)
    {
        g_clearAgain = false;
        if (g_mode == RGB_OFF) writeRgb(0, 0, 0);
    }

    float target[3] = {0.0f, 0.0f, 0.0f};

    if (g_mode != RGB_OFF) {
        const Preset& p = kPresets[g_mode];
        const uint32_t elapsed = now - g_modeAt;

        // 闪够次数就收工。这里必须真的把灯打黑：
        //   rgbOff() 只改状态（它的淡出靠后续 tick 完成），万一主循环被卡住，
        //   灯就会停在最后一帧的亮度上；所以紧跟一句 writeRgb(0,0,0)。
        //   写没写进去由返回值说话——这一行日志就是排障的关键。
        if (p.times != 0 && elapsed >= (uint32_t)p.times * p.period)
        {
            rgbOff(); // 状态归位：g_mode = RGB_OFF
            g_out[0] = g_out[1] = g_out[2] = 0.0f;
            const bool ok = writeRgb(0, 0, 0); // 真正灭灯：WS2812 只认这一帧
            g_clearAgain = true;
            logf("[LED] %s x%d done in %lu ms, off frame %s\n",
                 p.name, p.times, (unsigned long)elapsed, ok ? "sent" : "FAILED (retry)");
            return;
        }
        else
        {
            // 相位 0..1，升余弦 0->1->0；两端导数为 0，所以不会出现折点
            const float phase = (float)(elapsed % p.period) / (float)p.period;
            float amp = 0.5f - 0.5f * cosf(phase * 2.0f * (float)PI);

            // 留一点底光：最暗也有 floor%，观感是「变暗」而不是「关灯」
            const float lo = (float)p.floorPct * 0.01f;
            amp = lo + (1.0f - lo) * amp;

            const float k = amp * ((float)g_peak / 255.0f) * ((float)p.levelPct * 0.01f);
            target[0] = (float)p.r * k;
            target[1] = (float)p.g * k;
            target[2] = (float)p.b * k;
        }
    }

    // 朝目标缓动：既让闪的曲线保持光滑，也让「切换颜色/熄灭」变成渐变
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

    if (changed)
    {
        if (writeRgb(out[0], out[1], out[2]))
        {
            g_warned = false;
        }
        else if (!g_warned)
        {
            g_warned = true; // 只喊一次，失败会每 tick 自动重试
            logLine("[LED] write failed (bus busy), will retry");
        }
    }
}

#else // NETEINK_LED == 0：指示灯彻底关掉

/*
 * 一个字节都不发，也不初始化 RMT。只把数据脚拉成输出低电平：
 * WS2812 的空闲电平就是低，这样数据脚不会悬空被干扰出随机颜色，
 * 灯从冷启动到运行结束都保持熄灭。
 */
void rgbBegin(uint8_t pin, uint8_t)
{
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

void rgbSetMode(RgbMode)
{
}

void rgbOff()
{
}

void rgbTick()
{
}

#endif // NETEINK_LED
