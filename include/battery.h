/**
 * 电池电压（单节 3.7V 锂电，500mAh）
 * ----------------------------------------------------------------------------
 * 硬件：BAT+ -- R3 100K --+-- R4 220K -- GND，中间节点接 GPIO3（ADC1_CH2），
 *       并挂 C13 100nF 到地做低通。原理图见 docs/custom_esp32s2_schematic.svg。
 *
 *   V_adc = V_bat * 220/(100+220) = V_bat * 0.6875
 *   V_bat = V_adc * 1.4545
 *
 * 满电 4.2V 时 ADC 应为 2.89V，但分压电阻源阻抗偏高（68.75K）、ADC 采样窗
 * 又短，实测常比真值低一截。所以这里做「一点标定」：
 *   首次上电把当时的读数记为「满电」，存 NVS(ns "bat", key "full")；
 *   以后读到更高电压时自动往上修正，不会越用越偏。
 * 百分比先按 mV × 4200 / full 归一化，再查放电曲线——等价于把标定点拉到
 * 4.2V 的位置。想手动指定就填 kManualFullMv（非 0 时优先，且不再自动学）。
 *
 * 采样：analogReadMilliVolts() 走 IDF 的出厂校准，直接给 mV。
 * 读一次约 100ms（24 次 × 4ms 摊开，避开 WiFi 发射瞬间的压降），只在整屏
 * 刷新前调用。
 * ==========================================================================*/
#pragma once
#include <Arduino.h>

/** 配置 ADC 引脚与衰减，并读回上次的标定值；setup() 里调一次 */
void batteryBegin();

/**
 * 采一次电（约 100ms），返回**电池端**电压 mV（已乘回分压比）。
 * adcMv 非空时写入 ADC 引脚的原始电压，校准/排障用。
 */
int batteryMilliVolts(int* adcMv = nullptr);

/** 按放电曲线把 mV 换成 0..100 的百分比（内部按标定值归一化） */
int batteryPercent(int mv);

/** 当前用的「满电读数」mV；0 = 还没标定过（首次采样时会自动记） */
int batteryFullMilliVolts();
