/**
 *  @filename   :   epdif.h
 *  @brief      :   Header file of epdif.cpp providing EPD interface functions
 *                  Users have to implement all the functions in epdif.cpp
 *  @author     :   Yehui from Waveshare
 *
 *  Copyright (C) Waveshare     August 10 2017
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documnetation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to  whom the Software is
 * furished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef EPDIF_H
#define EPDIF_H

#include <Arduino.h>

/* ============================================================================
 * ESP32-S2 自画板引脚配置
 * ----------------------------------------------------------------------------
 * 依据 docs/custom_esp32s2_schematic.svg（U1 = ESP32-S2）里的网络标签，
 * 全部是 GPIO 编号（不是丝印 Dxx）：
 *
 *   原理图网络    GPIO   对应屏幕 / 用途
 *   -----------   ----   ----------------------------------------------
 *   SCLK          12     EPD CLK（SPI 时钟）
 *   MOSI          11     EPD DIN（SPI 数据，屏幕只写不读）
 *   (MISO)        10     空闲脚占位：SPI.begin() 需要一个 MISO，屏幕不回读
 *   EINK_CS        6     EPD CS
 *   EINK_DC        5     EPD DC
 *   EINK_RST       4     EPD RST
 *   EINK_BSY       7     EPD BUSY（0 = 忙，1 = 空闲）
 *   EINK_PWR       --    原理图上没有独立电源控制脚，故 EPD_PWR_PIN = -1
 *   LED            9     板载 WS2812 的 DIN（见 src/main.cpp 的 BOARD_RGB_PIN）
 *   KEY1 / KEY2   1 / 2  按键（本 demo 未使用）
 *   BAT_ADC        3     电池电压采样（本 demo 未使用）
 *
 * ESP32-S2 上要避开的引脚：
 *   GPIO 0 / 45 / 46 : 启动 strapping（0 已接 BOOT 键，46 仅输入）
 *   GPIO 19 / 20     : 原生 USB D- / D+（USB CDC 占用，COM9 走这里）
 *   GPIO 26 ~ 32     : 模组内部 Flash / PSRAM，不引出
 *
 * 接线有变化时只改这一段，其余代码无需改动。
 * EPD_PWR_PIN 设为 -1 表示不使用电源控制脚（屏幕由板载升压电路常供电）。
 * ==========================================================================*/
#define EPD_SCK_PIN     12
#define EPD_MISO_PIN    10   // 空闲脚占位：墨水屏只写不读，但 SPI.begin() 需要一个 MISO
#define EPD_MOSI_PIN    11
#define EPD_CS_PIN       6
#define EPD_DC_PIN       5
#define EPD_RST_PIN      4
#define EPD_BUSY_PIN     7
#define EPD_PWR_PIN     -1

// 保留原厂宏名，方便与 doc/epd1in54c 中的原始代码逐行对照
#define RST_PIN     EPD_RST_PIN
#define DC_PIN      EPD_DC_PIN
#define CS_PIN      EPD_CS_PIN
#define BUSY_PIN    EPD_BUSY_PIN
#define PWR_PIN     EPD_PWR_PIN

class EpdIf {
public:
    EpdIf(void);
    ~EpdIf(void);

    static int  IfInit(void);
    static void DigitalWrite(int pin, int value); 
    static int  DigitalRead(int pin);
    static void DelayMs(unsigned int delaytime);
    static void SpiTransfer(unsigned char data);
};

#endif
