/**
 * 城市表的运行时部分：当前选中的城市（NVS）、就近匹配、IP 定位。
 * ==========================================================================*/
#include "city.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

namespace
{
    int g_idx = 0; // CITY_TABLE 下标，永远有效

    // 老固件（只有 5 个预置城市）存的是下标，升级后按这张表迁移
    const uint32_t LEGACY_CODES[5] = {
        101020100, // 上海
        101190101, // 南京
        101150101, // 西宁
        101280601, // 深圳
        101150601, // 玉树
    };

    int findCode(uint32_t code)
    {
        for (int i = 0; i < CITY_COUNT; i++)
            if (CITY_TABLE[i].code == code) return i;
        return -1;
    }

    /** 取 JSON 里 "key":<数字> 的值 */
    bool numAfter(const String& s, const char* key, float& out)
    {
        const int i = s.indexOf(key);
        if (i < 0) return false;
        out = (float)atof(s.c_str() + i + strlen(key));
        return true;
    }

    bool httpJson(const char* url, String& body)
    {
        HTTPClient http;
        http.setTimeout(5000);
        http.setConnectTimeout(5000);
        if (!http.begin(url)) return false;
        const int code = http.GET();
        if (code != 200)
        {
            http.end();
            logf("[CITY] %s -> %d\n", url, code);
            return false;
        }
        body = http.getString();
        http.end();
        return body.length() > 20;
    }
} // namespace

void cityBegin()
{
    g_idx = findCode(101020100); // 兜底：上海
    if (g_idx < 0) g_idx = 0;

    Preferences p;
    if (!p.begin("ui", false)) return;

    const uint32_t code = p.getUInt("code", 0);
    if (code != 0)
    {
        const int i = findCode(code);
        if (i >= 0) g_idx = i;
        else logf("[CITY] code %lu not in table, fallback to Shanghai\n", (unsigned long)code);
    }
    else
    {
        const int old = p.getInt("city", -1);
        if (old >= 0 && old < 5)
        {
            const int i = findCode(LEGACY_CODES[old]);
            if (i >= 0)
            {
                g_idx = i;
                p.putUInt("code", LEGACY_CODES[old]);
                p.remove("city");
                logf("[CITY] migrate legacy index %d -> code %lu\n", old, (unsigned long)LEGACY_CODES[old]);
            }
        }
    }
    p.end();

    logf("[CITY] current code %lu (index %d of %d)\n",
         (unsigned long)CITY_TABLE[g_idx].code, g_idx, CITY_COUNT);
}

const CityRec& cityCurrent()
{
    return CITY_TABLE[g_idx];
}

const CityRec* cityByCode(uint32_t code)
{
    const int i = findCode(code);
    return i < 0 ? nullptr : &CITY_TABLE[i];
}

bool citySelect(uint32_t code)
{
    const int i = findCode(code);
    if (i < 0)
    {
        logf("[CITY] unknown city code %lu\n", (unsigned long)code);
        return false;
    }
    g_idx = i;

    Preferences p;
    if (p.begin("ui", false))
    {
        p.putUInt("code", code);
        p.end();
    }
    logf("[CITY] switched to code %lu\n", (unsigned long)code);
    return true;
}

const CityRec* cityNearest(float lat, float lon)
{
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) return nullptr;

    // 平面近似：经度按当前纬度的 cos 压缩。几百公里尺度上误差不到 0.5%，
    // 只用来「挑最近的一个城市」，不需要大圆距离。
    const float k = cosf(lat * 0.01745329f);
    float best = 1e9f;
    const CityRec* out = nullptr;
    for (int i = 0; i < CITY_COUNT; i++)
    {
        const float dy = lat - CITY_TABLE[i].lat / 100.0f;
        const float dx = (lon - CITY_TABLE[i].lon / 100.0f) * k;
        const float d = dx * dx + dy * dy;
        if (d < best)
        {
            best = d;
            out = &CITY_TABLE[i];
        }
    }
    if (out)
    {
        logf("[CITY] nearest code %lu, about %d km away\n",
             (unsigned long)out->code, (int)(sqrtf(best) * 111.0f));
    }
    return out;
}

const CityRec* cityLocateByIP()
{
    String body;
    float lat = 0, lon = 0;

    // 1) ip-api.com：免 key、纯 HTTP、UTF-8
    if (httpJson("http://ip-api.com/json/?fields=status,lat,lon,city", body) &&
        numAfter(body, "\"lat\":", lat) && numAfter(body, "\"lon\":", lon))
    {
        logf("[CITY] IP geo %d,%d (ip-api)\n", (int)(lat * 100), (int)(lon * 100));
        return cityNearest(lat, lon);
    }

    // 2) ipwho.is：备用源，字段名不同
    if (httpJson("http://ipwho.is/?fields=success,latitude,longitude,city", body) &&
        numAfter(body, "\"latitude\":", lat) && numAfter(body, "\"longitude\":", lon))
    {
        logf("[CITY] IP geo %d,%d (ipwho.is)\n", (int)(lat * 100), (int)(lon * 100));
        return cityNearest(lat, lon);
    }

    logLine("[CITY] IP geo failed (both services unreachable), pick manually");
    return nullptr;
}
