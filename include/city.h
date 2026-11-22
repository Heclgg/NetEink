/**
 * 城市：352 个城市（中国天气网编号 + 经纬度），表在 cities_data.h 里，只读不占 RAM。
 *
 * - 当前城市存 NVS("ui"/"code")，掉电不丢；从没存过就是上海。
 * - 老固件存的是「5 城下标」(NVS "ui"/"city")，首次上电会自动迁移过来。
 * - cityLocateByIP() 用公网出口 IP 定位，再按经纬度就近选城市：
 *   手机和板子连同一个 Wi-Fi，所以这就是「我人在哪儿」。
 * ==========================================================================*/
#ifndef CITY_H
#define CITY_H

#include <Arduino.h>

#include "cities_data.h"

/** 读 NVS，恢复上次选的城市；没有就用上海。setup() 里调一次。 */
void cityBegin();

/** 当前城市 */
const CityRec& cityCurrent();

/** 按编号找城市；找不到返回 nullptr */
const CityRec* cityByCode(uint32_t code);

/** 切换并写入 NVS；编号不在表里返回 false，当前城市不变 */
bool citySelect(uint32_t code);

/** 按经纬度找最近的城市（平面近似，误差 ≤ 0.5%）；表里没城市返回 nullptr */
const CityRec* cityNearest(float lat, float lon);

/**
 * 按公网 IP 定位：先问 ip-api.com，失败再问 ipwho.is；都失败返回 nullptr。
 * 调用前需已联网，整个过程会阻塞 3~8 秒。
 */
const CityRec* cityLocateByIP();

#endif // CITY_H
