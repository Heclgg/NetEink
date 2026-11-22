// 本文件由 tools/gen_cities.py 自动生成，请勿手工编辑。
// 数据: tools/data/cities.json（352 城 / 31 省），实现在 src/cities_data.cpp
// 城市码来自中国天气网，经纬度用于按定位就近选择。
#pragma once
#include <stdint.h>

struct CityRec
{
    const char* name; // UTF-8，最长 4 字
    uint32_t code; // 天气接口编号，如 101020100
    int16_t lon; // 经度 x100
    int16_t lat; // 纬度 x100
    uint8_t prov; // 省份下标，用于手机端分组
};

static constexpr int CITY_COUNT = 352;
static constexpr int CITY_PROVINCE_COUNT = 31;

extern const CityRec CITY_TABLE[CITY_COUNT];
extern const char* const CITY_PROVINCES[CITY_PROVINCE_COUNT];
