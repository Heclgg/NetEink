/**
 *  WiFi 配网：设备开热点 -> 手机扫码连上 -> captive portal 自动弹配置页
 *  -> 保存到 NVS -> 重启后连接。
 *
 *  只依赖 Arduino-ESP32 自带的 WiFi / WebServer / DNSServer / Preferences。
 */
#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <Arduino.h>

/** 从 NVS 读取已保存的凭据；返回 false 表示还没配过网 */
bool loadWifiCreds(String& ssid, String& pass);

/** 保存凭据到 NVS（namespace: wifi） */
void saveWifiCreds(const String& ssid, const String& pass);

/** 清除凭据（下次上电会重新进配网） */
void clearWifiCreds();

/** 默认热点名：NetEink-XXXX（MAC 末两字节） */
String defaultApSsid();

/** 启动 AP + DNS 劫持 + HTTP 服务 */
void startProvisioning(const String& apSsid, const String& apPass);

/** 阻塞运行配网；保存凭据并成功连上 WiFi 后返回 true（不重启设备） */
bool runProvisioning();

#endif  // PROVISIONING_H
