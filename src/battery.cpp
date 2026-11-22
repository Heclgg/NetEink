/**
 * 电池电压采样与一点标定，接口说明见 battery.h
 * ==========================================================================*/
#include "battery.h"

#include <Preferences.h>

#include "log.h"

namespace
{
    constexpr uint8_t kPin = 3; // BAT_ADC = GPIO3 = ADC1_CH2
    constexpr int kSamples = 24; // 摊开采样，避开 WiFi 发射瞬间的压降
    constexpr int kGapMs = 4; // 24 × 4ms ≈ 100ms

    // (100K + 220K) / 220K
    constexpr float kDivider = 320.0f / 220.0f;

    /**
     * 手动标定：填「电池满电时本机读到多少 mV」，比如 3921。
     * 填 0 = 自动标定（首次上电把当时读数记成满电，以后读到更高再往上修正）。
     */
    constexpr int kManualFullMv = 0;

    // 曲线以 4.2V 为 100%；标定只改「怎么把读数挪到这条曲线上」
    constexpr int kCurveTopMv = 4200;

    /**
     * 单节锂电的静置放电曲线（mV -> %）。
     * 平台期很平（3.7~3.9V 之间能差出 30%），所以不能拿电压线性当百分比。
     */
    const struct
    {
        uint16_t mv;
        uint8_t pct;
    } kCurve[] = {
        {4200, 100}, {4100, 92}, {4000, 82}, {3900, 70}, {3800, 55},
        {3700, 40}, {3650, 30}, {3600, 20}, {3500, 10}, {3400, 4},
        {3300, 0},
    };

    constexpr int kCurveCount = (int)(sizeof(kCurve) / sizeof(kCurve[0]));

    int g_fullMv = 0; // 满电读数；0 = 还没标定

    void saveFull(int mv)
    {
        Preferences p;
        if (p.begin("bat", false))
        {
            p.putUShort("full", (uint16_t)mv);
            p.end();
        }
    }

    /** 把一次读数记成满电（首次标定 / 往上修正） */
    void learnFull(int mv)
    {
        g_fullMv = mv;
        saveFull(mv);
        logf("[BAT] full = %d mV (saved to NVS)\n", mv);
    }
} // namespace

void batteryBegin()
{
    pinMode(kPin, INPUT);
    analogReadResolution(12);
    // 11dB：0~3.1V。分压后满电 2.89V，落在量程内。
    analogSetPinAttenuation(kPin, ADC_11db);
    (void)analogReadMilliVolts(kPin); // 丢掉第一次，ADC 刚使能时偏高

    if (kManualFullMv > 0)
    {
        g_fullMv = kManualFullMv;
        logf("[BAT] full = %d mV (manual)\n", g_fullMv);
        return;
    }

    Preferences p;
    if (p.begin("bat", true))
    {
        const uint16_t v = p.getUShort("full", 0);
        p.end();
        if (v >= 3000 && v <= 4400)
        {
            g_fullMv = v;
            logf("[BAT] full = %d mV (from NVS)\n", g_fullMv);
            return;
        }
    }
    logLine("[BAT] no calibration yet, the first reading becomes 100%");
}

int batteryMilliVolts(int* adcMv)
{
    uint32_t sum = 0;
    for (int i = 0; i < kSamples; i++)
    {
        sum += analogReadMilliVolts(kPin);
        delay(kGapMs);
    }
    const int raw = (int)((sum + kSamples / 2) / kSamples);
    if (adcMv) *adcMv = raw;

    const int mv = (int)(raw * kDivider + 0.5f);

    // 首次上电：把当前读数当成满电。以后真充满时读到更高，再往上修正一点，
    // 免得标定值一直偏低（+20mV 滞回，防止噪声来回改）。
    if (g_fullMv == 0) learnFull(mv);
    else if (mv > g_fullMv + 20) learnFull(mv);

    return mv;
}

int batteryPercent(int mv)
{
    if (g_fullMv > 0 && g_fullMv != kCurveTopMv)
    {
        // 一点标定：把「满电读数」拉伸到曲线的 4.2V 位置
        mv = (int)((long)mv * kCurveTopMv / g_fullMv);
    }

    if (mv >= kCurve[0].mv) return 100;
    if (mv <= kCurve[kCurveCount - 1].mv) return 0;

    for (int i = 1; i < kCurveCount; i++)
    {
        if (mv >= kCurve[i].mv)
        {
            const int hiMv = kCurve[i - 1].mv, loMv = kCurve[i].mv;
            const int hiPct = kCurve[i - 1].pct, loPct = kCurve[i].pct;
            return loPct + (mv - loMv) * (hiPct - loPct) / (hiMv - loMv);
        }
    }
    return 0;
}

int batteryFullMilliVolts()
{
    return g_fullMv;
}
