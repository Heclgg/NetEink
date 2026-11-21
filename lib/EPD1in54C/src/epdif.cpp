/**
 *  @filename   :   epdif.cpp
 *  @brief      :   Implements EPD interface functions
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "epdif.h"
#include <SPI.h>

EpdIf::EpdIf() {
};

EpdIf::~EpdIf() {
};

void EpdIf::DigitalWrite(int pin, int value) {
    digitalWrite(pin, value);
}

int EpdIf::DigitalRead(int pin) {
    return digitalRead(pin);
}

void EpdIf::DelayMs(unsigned int delaytime) {
    // Arduino 的 delay() 内部会 yield 给 FreeRTOS，不会饿死其它任务
    delay(delaytime);
}

void EpdIf::SpiTransfer(unsigned char data) {
    // IfInit() 里已经 beginTransaction() 并一直持有事务。
    // arduino-esp32 3.x 在持有事务时 transfer() 走 spiTransferByteNL()
    // （不再加/解一次锁），比每字节 begin/endTransaction 快得多。
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(data);
    digitalWrite(CS_PIN, HIGH);
}

int EpdIf::IfInit(void) {
    // 1) 片选先拉高：上电/复位期间 GPIO 处于高阻，避免误发第一个字节
    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, HIGH);
    pinMode(DC_PIN, OUTPUT);
    pinMode(BUSY_PIN, INPUT);

    // 2) 可选电源控制脚：部分驱动板用 MOS 管控制屏幕供电
#if EPD_PWR_PIN >= 0
    pinMode(PWR_PIN, OUTPUT);
    digitalWrite(PWR_PIN, HIGH);
    delay(10);   // 等屏幕电源稳定再发命令
#endif

    // 3) ESP32-S2 的 SPI 引脚由 GPIO 矩阵映射，必须显式指定。
    //    AVR 的 SPI 引脚是芯片固定的，SPI.begin() 不需要参数；
    //    S2 上不带参数会用 FSPI 默认 (SCK=36, MISO=37, MOSI=35, SS=34)，
    //    与本板原理图接线完全不同，所以必须把 epdif.h 里的引脚显式传进来。
    SPI.begin(EPD_SCK_PIN, EPD_MISO_PIN, EPD_MOSI_PIN, EPD_CS_PIN);

    // 4) 电子纸用 SPI MODE0 / MSB first / 2MHz。
    //    只 begin 一次并长期持有事务（原厂做法），不要每字节配对 endTransaction。
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    return 0;
}
