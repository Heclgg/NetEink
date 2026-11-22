/**
 * 设备网页：手机扫码 -> 选城市 / 传图
 * ----------------------------------------------------------------------------
 * 一个 WebServer + 一张内嵌 HTML，两块功能同页：
 *   城市：搜索 352 个城市，或按 IP 定位就近自动选（见 city.h）
 *   传图：页内裁剪/缩放/抖动 -> 上传 -> 墨水屏显示
 *
 * 为什么传图要「在手机端抖动」：
 *   这块屏只认 1-bit 的黑/红两层位图，没有灰度。图像处理（裁剪、缩放、
 *   误差扩散）放在浏览器里做，设备端就不用背 JPEG 解码器、不用 90KB 的
 *   灰度缓冲，也不用为三色量化再写一遍 C++——网页和 tools/gen_ui_assets.py
 *   用同一套 Floyd–Steinberg 算法，效果一致。
 *   传输量只有 152*152/8*2 = 5776 字节，一次 POST 就完。
 *
 * 协议：
 *   GET  /          返回内嵌网页（PROGMEM）
 *   GET  /cities    {"n":"当前城市","c":编号,"p":["省",…],"l":[["市",编号,省下标],…]}
 *   POST /city      body = 城市编号 -> 存入 NVS，稍后回天气页刷新
 *   POST /geo       ?lat=&lon=（可选，手机给了坐标就不用 IP）
 *                   否则按公网 IP 定位 -> 就近选城市，同上
 *   POST /upload    body = 5776 字节点阵的 base64：前 2888 黑层，后 2888 红层
 *                   每位 1 = 该颜色着色，MSB 在左，19 字节/行
 *   GET  /status    {"busy":0|1,"count":n}
 *
 * 收到的帧由 main.cpp 刷完屏后另存一份到 Flash（见 photo_store.cpp），
 * 之后长按 KEY2 就能不联网重放这一张。
 * ==========================================================================*/
#pragma once
#include <Arduino.h>

#define PHOTO_W            152
#define PHOTO_H            152
#define PHOTO_FRAME_BYTES  (PHOTO_W * PHOTO_H / 8)   // 2888
#define PHOTO_BODY_BYTES   (PHOTO_FRAME_BYTES * 2)   // 5776

/** 启动网页服务（要求 WiFi 已连上）。重复调用无副作用。 */
bool photoServerStart();

/** 必须在主循环里反复调用：WebServer 不是后台任务，靠它处理请求 */
void photoServerLoop();

/** 停掉服务并释放 80 端口、恢复 WiFi 省电 */
void photoServerStop();

/** 当前访问地址，例如 http://192.168.1.23/；forCity 时带 #city 让页面滚到城市区 */
String photoServerUrl(bool forCity = false);

/**
 * 取一帧待显示的图：有新图时写入 black/red（已按屏幕极性取反）并置忙。
 * 调用后应尽快刷屏，刷完调 photoMarkDone()。
 */
bool photoTakeFrame(uint8_t * black, uint8_t * red);

/** 刷屏结束，允许接收下一张 */
void photoMarkDone();

/** 已收到的图片张数 */
uint32_t photoFrameCount();

/**
 * 手机改过城市了吗？改过后约 1.2 秒返回一次 true（留时间把 HTTP 响应发完），
 * 调用方随后应该停网页、回天气页刷新。
 */
bool photoCityChangeDue();
