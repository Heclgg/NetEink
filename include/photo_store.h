/**
 * 上一次传的图：落在 Flash 里，断电不丢
 * ----------------------------------------------------------------------------
 * 只存一张 152x152 的双层点阵（黑层 2888 + 红层 2888 = 5776 字节），
 * 长按 KEY2 时直接读出来刷屏，不用连 WiFi、不用手机。
 *
 * 参数用屏幕极性（0 = 该层着色），和 g_black / g_red 一模一样，
 * 读出来 memcpy 进帧缓冲就能刷，中间不做任何转换。
 * ==========================================================================*/
#pragma once
#include <stdint.h>

/** 存一张：black/red 各 2888 字节，屏幕极性。失败返回 false（不影响已存的那张）。 */
bool photoStoreSave(const uint8_t* black, const uint8_t* red);

/** 取最近存的那张，写进 black/red。没存过 / 校验不过返回 false。 */
bool photoStoreLoad(uint8_t * black, uint8_t * red);
