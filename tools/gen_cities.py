#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 tools/data/cities.json 转成固件用的城市表。

用法: python tools/gen_cities.py
输出: include/cities_data.h（声明） + src/cities_data.cpp（数据）

为什么拆两个文件：表只写头文件的话，凡是 include 它的 .cpp 都会存一份副本
（实测 main.cpp 和 photo_server.cpp 各一份，白白多占 9KB Flash）。
现在头文件只有 extern 声明，数据只在 cities_data.cpp 里出现一次。

名字用 UTF-8 字符串，城市码存整数（101020100），经纬度按 0.01 度存成
int16（±180 度 -> ±18000，装得下），省份存下标。一条记录 16 字节，
352 条连名字约 8KB，全在 .rodata 里，不占 RAM。
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "data", "cities.json")
HDR = os.path.join(ROOT, "include", "cities_data.h")
CPP = os.path.join(ROOT, "src", "cities_data.cpp")


def main():
    with open(SRC, encoding="utf-8") as f:
        data = json.load(f)

    provs = data["provinces"]
    cities = data["cities"]

    # 排序：按省份下标，省内保持原顺序（原顺序就是天气网的「省会 -> 地级市」）
    cities = sorted(cities, key=lambda c: c["p"])

    for c in cities:
        assert 0 <= c["p"] < len(provs), c
        assert 10000000 <= c["c"] <= 999999999, c
        assert -18000 <= round(c["lon"] * 100) <= 18000, c
        assert -9000 <= round(c["lat"] * 100) <= 9000, c
        assert '"' not in c["n"] and "\\" not in c["n"], c

    hdr = []
    hdr.append("// 本文件由 tools/gen_cities.py 自动生成，请勿手工编辑。")
    hdr.append("// 数据: tools/data/cities.json（%d 城 / %d 省），实现在 src/cities_data.cpp"
               % (len(cities), len(provs)))
    hdr.append("// 城市码来自中国天气网，经纬度用于按定位就近选择。")
    hdr.append("#pragma once")
    hdr.append("#include <stdint.h>")
    hdr.append("")
    hdr.append("struct CityRec {")
    hdr.append("    const char* name;    // UTF-8，最长 4 字")
    hdr.append("    uint32_t    code;    // 天气接口编号，如 101020100")
    hdr.append("    int16_t     lon;     // 经度 x100")
    hdr.append("    int16_t     lat;     // 纬度 x100")
    hdr.append("    uint8_t     prov;    // 省份下标，用于手机端分组")
    hdr.append("};")
    hdr.append("")
    hdr.append("static constexpr int CITY_COUNT = %d;" % len(cities))
    hdr.append("static constexpr int CITY_PROVINCE_COUNT = %d;" % len(provs))
    hdr.append("")
    hdr.append("extern const CityRec CITY_TABLE[CITY_COUNT];")
    hdr.append("extern const char* const CITY_PROVINCES[CITY_PROVINCE_COUNT];")
    hdr.append("")

    cpp = []
    cpp.append("// 本文件由 tools/gen_cities.py 自动生成，请勿手工编辑。")
    cpp.append("#include \"cities_data.h\"")
    cpp.append("")
    cpp.append("const CityRec CITY_TABLE[CITY_COUNT] = {")
    for c in cities:
        cpp.append('    {"%s", %d, %d, %d, %d},'
                   % (c["n"], c["c"], round(c["lon"] * 100), round(c["lat"] * 100), c["p"]))
    cpp.append("};")
    cpp.append("")
    cpp.append("const char* const CITY_PROVINCES[CITY_PROVINCE_COUNT] = {")
    for i in range(0, len(provs), 10):
        cpp.append("    " + " ".join('"%s",' % p for p in provs[i:i + 10]))
    cpp.append("};")
    cpp.append("")

    for path, lines in ((HDR, hdr), (CPP, cpp)):
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))

    names = "".join(c["n"] for c in cities)
    print("写出 %s / %s" % (HDR, CPP))
    print("  城市 %d / 省份 %d / 汉字 %d / 头文件 %d 字节 / 实现 %d 字节"
          % (len(cities), len(provs), len(set(names)),
             os.path.getsize(HDR), os.path.getsize(CPP)))


if __name__ == "__main__":
    main()
