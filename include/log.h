/**
 * 统一日志出口：默认双路输出（原生 USB CDC = COM9，UART0 = GPIO43/44）。
 * ----------------------------------------------------------------------------
 * release 构建加 -DNETEINK_LOG=0，两个宏会展开成空语句。
 * 为什么用宏而不是空函数：空函数的参数仍要求值是合法的字符串字面量，
 * 那些字面量会原样留在 Flash 里；宏展开成 (void)0 后字面量被彻底丢弃。
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <stdarg.h>

#ifndef NETEINK_LOG
#define NETEINK_LOG 1       // 默认开；platformio.ini 的 release env 会置 0
#endif

#if NETEINK_LOG

/** printf 风格，一行一条。缓冲 224 字节对现有日志（最长约 120 字节）足够 */
static inline void logfImpl(const char* fmt, ...) __attribute__
((format
(printf
,
1
,
2
)
)
);

static inline void logfImpl(const char* fmt, ...)
{
    char buf[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
    Serial0.print(buf);
}

static inline void logLineImpl(const char* s = "")
{
    Serial.println(s);
    Serial0.println(s);
}

/** 串口初始化。release 下不执行，USB CDC 仍由 ARDUINO_USB_CDC_ON_BOOT 枚举 */
static inline void logBegin(unsigned long baud = 115200)
{
    Serial.begin(baud);
    Serial0.begin(baud);
}

#define logf(...)       logfImpl(__VA_ARGS__)
#define logLine(...)    logLineImpl(__VA_ARGS__)

#else /* NETEINK_LOG == 0：全部编译掉 */

#define logf(...)       ((void)0)
#define logLine(...)    ((void)0)
#define logBegin(...)   ((void)0)

#endif
