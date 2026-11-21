#include "provisioning.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <nvs_flash.h>

#include "rgb_led.h"

static WebServer g_server(80);
static DNSServer g_dns;

/* ============================================================================
 * NVS 读写
 * ==========================================================================*/
static const char* NVS_NS = "wifi";

bool loadWifiCreds(String& ssid, String& pass) {
    // NVS 分区自检：返回非 0 说明分区未初始化或需要擦除重建
    const esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        Serial.printf("[NVS] flash_init 失败: %d (%s)\n", static_cast<int>(err), esp_err_to_name(err));
    }

    Preferences p;
    // 用读写模式打开：命名空间不存在时会自动创建，
    // 只读模式（true）在首次配网前必然失败。
    if (!p.begin(NVS_NS, false)) {
        Serial.printf("[NVS] begin 失败: %s\n", esp_err_to_name(err));
        return false;
    }
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
    Serial.printf("[NVS] 读出 ssid=\"%s\" (%u 字节), pass %u 字节\n",
                  ssid.c_str(), (unsigned)ssid.length(), (unsigned)pass.length());
    return ssid.length() > 0;
}

void saveWifiCreds(const String& ssid, const String& pass) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        Serial.println("[NVS] begin 失败，凭据没有保存");
        return;
    }
    const size_t n1 = p.putString("ssid", ssid);
    const size_t n2 = p.putString("pass", pass);
    p.end();                                     // end() 里才真正 commit
    Serial.printf("[NVS] 写入 ssid=%u 字节, pass=%u 字节\n",
                  static_cast<unsigned>(n1), static_cast<unsigned>(n2));
}

void clearWifiCreds() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.clear();
    p.end();
}

String defaultApSsid() {
    uint8_t mac[6] = {0};
    // 注意：WiFi.macAddress() 在 WiFi 还没初始化时返回全 0，
    // 会得到 NetEink-0000 这种不唯一的 AP 名，所以直接读 eFuse。
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[24];
    snprintf(buf, sizeof(buf), "NetEink-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

/* ============================================================================
 * 配置页：移动端优先，纯内联样式，不依赖外部资源
 * 注意：raw string 的结束标记不能出现在内容里
 * ==========================================================================*/
static const char PAGE_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NetEink · 配网</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:20px 16px 40px;background:#f4f2ec;color:#222;
     font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",sans-serif}
.card{max-width:430px;margin:0 auto;background:#fff;border-radius:16px;padding:22px 20px 24px;
      box-shadow:0 4px 20px rgba(0,0,0,.06)}
h1{font-size:20px;margin:0 0 6px;font-weight:600}
.sub{color:#777;font-size:13px;margin:0 0 20px;line-height:1.5}
label{display:block;font-size:13px;color:#555;margin:16px 0 7px}
input{width:100%;padding:13px 14px;border:1px solid #ddd;border-radius:11px;font-size:16px;
      outline:none;background:#fafafa}
input:focus{border-color:#c0392b;background:#fff}
button{width:100%;margin-top:22px;padding:15px;border:0;border-radius:11px;
       background:#c0392b;color:#fff;font-size:16px;font-weight:500}
button:active{background:#a93226}
h2{font-size:13px;color:#555;font-weight:500;margin:24px 0 8px}
.net{display:flex;justify-content:space-between;align-items:center;padding:13px 4px;
     border-bottom:1px solid #f0f0f0;font-size:15px}
.net:active{background:#faf7f5}
.net small{color:#999;font-size:12px}
.empty{color:#999;font-size:13px;padding:10px 4px}
</style></head>
<body>
<div class="card">
  <h1>连接 WiFi</h1>
  <p class="sub">请选择 2.4GHz 网络。保存后设备会自动重启并开始显示天气。</p>
  <form method="get" action="/save">
    <label>网络名称</label>
    <input name="ssid" id="ssid" required autocomplete="off" autocapitalize="none"
           placeholder="点击下方网络，或手动输入">
    <label>密码</label>
    <input name="pass" type="password" autocomplete="off" placeholder="WiFi 密码">
    <button type="submit">保存并重启</button>
  </form>
  <h2>附近的网络</h2>
  <div id="list"><div class="empty">扫描中…</div></div>
</div>
<script>
fetch('/scan').then(function (r) { return r.json(); }).then(function (aps) {
  var box = document.getElementById('list');
  if (!aps || !aps.length) {
    box.innerHTML = '<div class="empty">没有扫描到网络，请靠近路由器后刷新页面</div>';
    return;
  }
  box.innerHTML = aps.map(function (a) {
    return '<div class="net" data-s="' + a.s + '"><span>' + a.s +
           '</span><small>' + a.r + ' dBm</small></div>';
  }).join('');
  Array.prototype.forEach.call(box.querySelectorAll('.net'), function (el) {
    el.onclick = function () {
      document.getElementById('ssid').value = el.getAttribute('data-s');
      document.getElementById('pass').focus();
    };
  });
}).catch(function () {
  document.getElementById('list').innerHTML = '<div class="empty">扫描失败</div>';
});
</script>
</body></html>
)rawliteral";

/* 保存后的过渡页：每秒轮询 /status，实时显示连接结果，不再依赖重启 */
static constexpr char PAGE_CONNECTING[] = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{margin:0;padding:44px 24px;background:#f4f2ec;color:#222;
     font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC",sans-serif;
     text-align:center}
.mark{font-size:46px;line-height:1;margin-bottom:16px;color:#c0392b}
h3{font-size:19px;margin:0 0 10px;font-weight:600}
p{color:#777;font-size:14px;line-height:1.8;margin:0}
</style></head>
<body>
<div class="mark" id="mk">&#9679;</div>
<h3 id="t">正在连接…</h3>
<p id="d">请让手机靠近设备，约需 10~20 秒</p>
<script>
var timer = setInterval(function () {
  fetch('/status').then(function (r) { return r.json(); }).then(function (o) {
    if (o.s === 3) {
      document.getElementById('mk').innerHTML = '&#10003;';
      document.getElementById('t').textContent = '连接成功';
      document.getElementById('d').textContent = '设备即将刷新屏幕显示天气';
      clearInterval(timer);
    } else if (o.s === 4) {
      document.getElementById('mk').innerHTML = '&#10007;';
      document.getElementById('t').textContent = '连接失败';
      document.getElementById('d').innerHTML = '请检查密码是否正确<br>返回上一页可重新配置';
      clearInterval(timer);
    }
  }).catch(function () {});
}, 1200);
</script>
</body></html>
)rawliteral";

/* ============================================================================
 * HTTP 处理
 * ==========================================================================*/
static void handleRoot() {
    g_server.send_P(200, "text/html", PAGE_HTML);
}

/**
 * 未注册路径统一 302 回配置页。
 * iOS 连上 AP 后会探测 captive.apple.com/hotspot-detect.html，
 * Android 探测 connectivitycheck.gstatic.com/generate_204，
 * 返回 302 才会被系统判定为「需要登录的网络」并自动弹出配置页。
 */
static void handleNotFound() {
    g_server.sendHeader("Location", "/", true);
    g_server.send(302, "text/plain", "");
}

static void handleScan() {
    // AP 模式下扫描会让已连接的手机短暂卡顿，属正常现象
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n && i < 18; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;          // 跳过隐藏 SSID
        ssid.replace("\\", "");
        ssid.replace("\"", "");
        if (json.length() > 1) json += ",";
        json += "{\"s\":\"" + ssid + "\",\"r\":" + String((int)WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    g_server.send(200, "application/json", json);
}

static String           g_pendingSsid;
static String           g_pendingPass;
static volatile uint8_t g_saveState = 0;   // 0 空闲 1 待连接 2 连接中 3 成功 4 失败

static void handleSave() {
    const String ssid = g_server.arg("ssid");
    const String pass = g_server.arg("pass");
    if (ssid.length() == 0) {
        g_server.send(400, "text/plain", "SSID 不能为空");
        return;
    }
    Serial.printf("[AP] 收到 ssid=\"%s\" (长度 %u), pass 长度 %u\n",
                  ssid.c_str(), (unsigned)ssid.length(), (unsigned)pass.length());
    saveWifiCreds(ssid, pass);

    g_pendingSsid = ssid;
    g_pendingPass = pass;
    g_saveState = 1;                     // 真正的连接交给 runProvisioning
    g_server.send_P(200, "text/html", PAGE_CONNECTING);
}

static void handleStatus() {
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"s\":%u}", (unsigned)g_saveState);
    g_server.send(200, "application/json", buf);
}

/* ============================================================================
 * 对外接口
 * ==========================================================================*/
void startProvisioning(const String& apSsid, const String& apPass) {
    // 用 AP+STA 共存模式：保存后可以直接 WiFi.begin() 连新网络，
    // 不必先关热点再切模式 —— 那一步在 S2 上容易把射频卡死。
    WiFiClass::mode(WIFI_AP_STA);
    WiFi.softAP(apSsid.c_str(), apPass.c_str());
    delay(200);

    const IPAddress ip = WiFi.softAPIP();
    Serial.printf("[AP] 热点 %s / %s  IP=%s\n",
                  apSsid.c_str(), apPass.c_str(), ip.toString().c_str());

    g_dns.start(53, "*", ip);

    g_server.on("/", handleRoot);
    g_server.on("/scan", handleScan);
    g_server.on("/save", handleSave);
    g_server.on("/status", handleStatus);
    g_server.onNotFound(handleNotFound);
    g_server.begin();
}

/**
 * 阻塞运行配网。保存凭据后直接连新 WiFi，连上才关热点返回。
 * 返回 true 表示 STA 已连接，调用方可以直接去拉天气。
 */
bool runProvisioning() {
    for (;;) {
        g_dns.processNextRequest();
        g_server.handleClient();

        if (g_saveState == 1) {
            g_saveState = 2;
            Serial.printf("[AP] 连接 %s ...\n", g_pendingSsid.c_str());
            WiFi.begin(g_pendingSsid.c_str(), g_pendingPass.c_str());
            rgbSetMode(RGB_WAIT);             // 正在连用户给的新网络：蓝呼吸

            const uint32_t t0 = millis();
            while (millis() - t0 < 20000) {
                g_dns.processNextRequest();   // 连接期间页面仍可轮询 /status
                g_server.handleClient();
                if (WiFi.status() == WL_CONNECTED) {
                    g_saveState = 3;
                    Serial.printf("[AP] 连接成功 %s\n",
                                  WiFi.localIP().toString().c_str());
                    break;
                }
                rgbTick();                    // 20s 等待期间呼吸不能停
                delay(20);
            }
            if (g_saveState != 3) {
                g_saveState = 4;
                Serial.printf("[AP] 连接失败 status=%d\n", (int)WiFi.status());
                WiFi.disconnect();
                rgbSetMode(RGB_CONFIG);       // 失败：回到绿色慢呼吸继续等
            }
        }

        if (g_saveState == 3) {
            // 让页面有机会轮询到「成功」，期间继续推呼吸灯
            const uint32_t t0 = millis();
            while (millis() - t0 < 2000) {
                rgbTick();
                delay(20);
            }
            g_server.stop();
            g_dns.stop();
            WiFi.softAPdisconnect(true);      // 关热点，把射频让给 STA
            Serial.println("[AP] 配网完成，关闭热点");
            return true;
        }

        rgbTick();
        delay(2);
    }
}
