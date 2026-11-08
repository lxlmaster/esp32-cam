/*
 * ESP32-S3-Cam 家庭摄像头（GC2145）v3 —— 流水线版
 * ============================================================
 * v2 的问题：抓帧 -> 压缩 -> 发送 全在一个 HTTP 请求里串行做，
 *           一次请求要 200-800ms，帧率上不去。
 *
 * v3 改成生产者-消费者：
 *   camTask（独立任务，跑在另一个核）：
 *       不停抓帧 -> frame2jpg -> 写进共享缓冲
 *   HTTP handler：
 *       只从共享缓冲拷一份就走，不等压缩
 *
 * 效果：压缩时间和网络传输重叠，帧率接近压缩速度的上限。
 *
 * 另外把 AWB / 自动增益 / 自动曝光 默认打开
 * （GC2145 上电不做色彩校正，不开 AWB 画面是绿的）
 *
 * 调参接口（带 Basic Auth）：
 *   /set?size=qvga|cif|hvga|vga|svga
 *   /set?q=10            JPEG 质量 0-63（越小越清晰越慢）
 *   /set?wb=0|1          自动白平衡
 *   /set?br=-2..2        亮度
 *   /set?ct=-2..2        对比度
 *   /set?st=-2..2        饱和度
 *   /set                查看状态
 *
 * 板子设置：ESP32S3 Dev / Flash 8MB / PSRAM OPI / Huge APP / CDC 关
 * ============================================================
 */

#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ==================== 配置 ==================== */
const char *WIFI_SSID = "1001";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char *AUTH_USER = "cam";
const char *AUTH_PASS = "YOUR_CAM_PASSWORD";

/* ---------- 引脚（官方 ESP32S3-EYE） ---------- */
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

#define XCLK_FREQ       15000000

/* ---------- 运行参数 ---------- */
static framesize_t g_fs      = FRAMESIZE_CIF;   /* 400x296：实测 124ms / 8fps，画质与流畅的平衡点 */
static int         g_quality = 10;
static bool        g_inited  = false;

/* ---------- 软件色彩校正 ----------
 * GC2145 驱动的 set_whitebal / set_brightness / set_saturation 全是空函数，
 * 没法靠 API 修色。所以在 frame2jpg 之前直接改 RGB565 像素。
 * 增益是 8.8 定点数：256 = 1.0 倍，280 ≈ 1.09 倍，200 ≈ 0.78 倍
 */
static uint16_t g_rGain = 256;
static uint16_t g_gGain = 256;   /* 默认不校色：实测原始画面正常，压绿反而搞坏 */
static uint16_t g_bGain = 256;
static bool     g_colorFix = false;

/* 查表：R 5bit(32) G 6bit(64) B 5bit(32)。
   比逐像素乘法快不少，且省掉 clamp 分支。 */
static uint16_t g_rLUT[32];
static uint16_t g_gLUT[64];
static uint16_t g_bLUT[32];

static void rebuildLUT() {
  for (int i = 0; i < 32; i++) {
    uint32_t v = ((uint32_t)i * g_rGain) >> 8;
    g_rLUT[i] = (v > 31) ? 31 : (uint16_t)v;
  }
  for (int i = 0; i < 64; i++) {
    uint32_t v = ((uint32_t)i * g_gGain) >> 8;
    g_gLUT[i] = (v > 63) ? 63 : (uint16_t)v;
  }
  for (int i = 0; i < 32; i++) {
    uint32_t v = ((uint32_t)i * g_bGain) >> 8;
    g_bLUT[i] = (v > 31) ? 31 : (uint16_t)v;
  }
}

static inline void applyColorFix(uint8_t *buf, size_t bytes) {
  if (!g_colorFix) return;
  uint16_t *p = (uint16_t *)buf;
  size_t n = bytes / 2;
  for (size_t i = 0; i < n; i++) {
    uint16_t v = p[i];
    p[i] = (uint16_t)((g_rLUT[(v >> 11) & 0x1F] << 11) |
                      (g_gLUT[(v >> 5)  & 0x3F] << 5)  |
                       g_bLUT[ v        & 0x1F]);
  }
}

/* ---------- 共享帧缓冲 ---------- */
static uint8_t         *g_jpgBuf   = NULL;   /* PSRAM 里的当前帧 */
static volatile size_t  g_jpgLen   = 0;
static SemaphoreHandle_t g_bufLock = NULL;
static volatile bool    g_camRun   = false;
static volatile uint32_t g_frameCount = 0;
static volatile uint32_t g_lastCostMs = 0;

static httpd_handle_t g_server = NULL;
static char g_authHeader[128];

/* ---------- 认证 ---------- */

static void buildAuthHeader() {
  char plain[96];
  snprintf(plain, sizeof(plain), "%s:%s", AUTH_USER, AUTH_PASS);
  unsigned char b64[160];
  size_t olen = 0;
  mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen,
                        (const unsigned char *)plain, strlen(plain));
  b64[olen] = '\0';
  snprintf(g_authHeader, sizeof(g_authHeader), "Basic %s", (char *)b64);
}

static bool checkAuth(httpd_req_t *req) {
  char hdr[128];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
    return false;
  return strcmp(hdr, g_authHeader) == 0;
}

static void sendAuthRequired(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-CAM\"");
  httpd_resp_send(req, "auth required", HTTPD_RESP_USE_STRLEN);
}

/* ---------- 摄像头初始化 ---------- */

static bool startCamera(framesize_t fs, int quality) {
  g_camRun = false;
  delay(200);

  if (g_inited) {
    esp_camera_deinit();
    g_inited = false;
    delay(150);
  }

  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_pwdn     = PWDN_GPIO_NUM;
  c.pin_reset    = RESET_GPIO_NUM;
  c.pin_xclk     = XCLK_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href  = HREF_GPIO_NUM;
  c.pin_pclk  = PCLK_GPIO_NUM;

  c.xclk_freq_hz = XCLK_FREQ;
  c.pixel_format = PIXFORMAT_RGB565;
  c.frame_size   = fs;
  c.jpeg_quality = quality;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] 初始化失败 0x%x\n", err);
    return false;
  }
  g_inited = true;
  g_fs = fs;
  g_quality = quality;

  /* 默认开全套自动控制 —— 不开 AWB 画面是绿的 */
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_whitebal(s, 1);       /* 自动白平衡 */
    s->set_awb_gain(s, 1);       /* 白平衡增益 */
    s->set_gain_ctrl(s, 1);      /* 自动增益 */
    s->set_exposure_ctrl(s, 1);  /* 自动曝光 */
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    Serial.println("[CAM] AWB/AGC/AEC 已开启");
  }

  g_camRun = true;

  int w = 0, h = 0;
  switch (fs) {
    case FRAMESIZE_QVGA: w = 320; h = 240; break;
    case FRAMESIZE_CIF:  w = 400; h = 296; break;
    case FRAMESIZE_HVGA: w = 480; h = 320; break;
    case FRAMESIZE_VGA:  w = 640; h = 480; break;
    case FRAMESIZE_SVGA: w = 800; h = 600; break;
    case FRAMESIZE_XGA:  w = 1024; h = 768; break;
    default: break;
  }
  Serial.printf("[CAM] %dx%d quality=%d\n", w, h, quality);
  return true;
}

/* ---------- 生产者任务：不停抓帧压缩 ---------- */

static void camTask(void *arg) {
  Serial.println("[TASK] 采集任务启动");
  while (true) {
    if (!g_camRun || !g_inited) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

    int64_t t0 = esp_timer_get_time();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

    /* 压缩之前先在原始像素上校色 */
    if (fb->format == PIXFORMAT_RGB565) {
      size_t pxBytes = (size_t)fb->width * fb->height * 2;
      if (pxBytes <= fb->len) applyColorFix(fb->buf, pxBytes);
    }

    uint8_t *jpg = NULL;
    size_t len = 0;
    bool ok = frame2jpg(fb, g_quality, &jpg, &len);
    esp_camera_fb_return(fb);

    if (ok && jpg && len > 0 && g_jpgBuf) {
      if (xSemaphoreTake(g_bufLock, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(g_jpgBuf, jpg, len);
        g_jpgLen = len;
        g_frameCount++;
        xSemaphoreGive(g_bufLock);
      }
      g_lastCostMs = (esp_timer_get_time() - t0) / 1000;
    }
    if (jpg) free(jpg);

    /* 让出 CPU，避免把另一个核饿死 */
    vTaskDelay(1);
  }
}

/* ---------- 取一份当前帧 ---------- */

static bool fetchJpeg(uint8_t **out, size_t *outLen) {
  if (g_jpgLen == 0 || !g_jpgBuf) return false;
  uint8_t *copy = (uint8_t *)malloc(g_jpgLen);
  if (!copy) return false;

  if (xSemaphoreTake(g_bufLock, pdMS_TO_TICKS(200)) != pdTRUE) {
    free(copy);
    return false;
  }
  memcpy(copy, g_jpgBuf, g_jpgLen);
  size_t n = g_jpgLen;
  xSemaphoreGive(g_bufLock);

  *out = copy;
  *outLen = n;
  return true;
}

/* ---------- /snap ---------- */

static esp_err_t snapHandler(httpd_req_t *req) {
  if (!checkAuth(req)) { sendAuthRequired(req); return ESP_OK; }

  uint8_t *jpg = NULL;
  size_t len = 0;
  if (!fetchJpeg(&jpg, &len)) { httpd_resp_send_500(req); return ESP_FAIL; }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache");
  esp_err_t ret = httpd_resp_send(req, (const char *)jpg, len);
  free(jpg);
  return ret;
}

/* ---------- /stream ---------- */

#define PART_BOUNDARY "frame"
static const char *STREAM_CT       = "multipart/x-mixed-replace; boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART     = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
  if (!checkAuth(req)) { sendAuthRequired(req); return ESP_OK; }

  esp_err_t res = httpd_resp_set_type(req, STREAM_CT);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache");

  char partBuf[64];
  uint32_t lastSeq = 0;

  while (true) {
    /* 等新帧，避免同一帧重复发 */
    uint32_t waited = 0;
    while (g_frameCount == lastSeq && waited < 200) {
      vTaskDelay(pdMS_TO_TICKS(10));
      waited += 10;
    }
    lastSeq = g_frameCount;

    uint8_t *jpg = NULL;
    size_t len = 0;
    if (!fetchJpeg(&jpg, &len)) break;

    size_t hlen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, partBuf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpg, len);
    free(jpg);
    if (res != ESP_OK) break;
  }
  return res;
}

/* ---------- /set ---------- */

static bool getParam(httpd_req_t *req, const char *key, char *val, size_t valLen) {
  char buf[192];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) return false;
  return httpd_query_key_value(buf, key, val, valLen) == ESP_OK;
}

static esp_err_t setHandler(httpd_req_t *req) {
  if (!checkAuth(req)) { sendAuthRequired(req); return ESP_OK; }

  char val[24];
  bool needRestart = false;
  framesize_t newFs = g_fs;
  int newQ = g_quality;

  if (getParam(req, "size", val, sizeof(val))) {
    if      (strcasecmp(val, "qvga") == 0) newFs = FRAMESIZE_QVGA;
    else if (strcasecmp(val, "cif")  == 0) newFs = FRAMESIZE_CIF;
    else if (strcasecmp(val, "hvga") == 0) newFs = FRAMESIZE_HVGA;
    else if (strcasecmp(val, "vga")  == 0) newFs = FRAMESIZE_VGA;
    else if (strcasecmp(val, "svga") == 0) newFs = FRAMESIZE_SVGA;
    else if (strcasecmp(val, "xga")  == 0) newFs = FRAMESIZE_XGA;
    needRestart = true;
  }

  if (getParam(req, "q", val, sizeof(val))) {
    newQ = atoi(val);
    if (newQ < 0)  newQ = 0;
    if (newQ > 63) newQ = 63;
    needRestart = true;
  }

  if (needRestart) {
    if (!startCamera(newFs, newQ)) startCamera(FRAMESIZE_QVGA, 12);
    delay(250);
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    /* 注意：GC2145 驱动把这些全设成 set_dummy 空函数，调了也没用。
       留着是为了将来换驱动时兼容。 */
    if (getParam(req, "wb", val, sizeof(val)))       s->set_whitebal(s, atoi(val));
    if (getParam(req, "br", val, sizeof(val)))       s->set_brightness(s, atoi(val));
    if (getParam(req, "ct", val, sizeof(val)))       s->set_contrast(s, atoi(val));
    if (getParam(req, "st", val, sizeof(val)))       s->set_saturation(s, atoi(val));
    if (getParam(req, "vflip", val, sizeof(val)))    s->set_vflip(s, atoi(val));
    if (getParam(req, "hmirror", val, sizeof(val)))  s->set_hmirror(s, atoi(val));

    /* 彩条测试：验证 RGB 通道顺序对不对，这个能绕过白平衡问题 */
    if (getParam(req, "colorbar", val, sizeof(val)))
      s->set_colorbar(s, atoi(val) ? 1 : 0);

    /* 直接写寄存器（绕过驱动的空实现）
       /reg?page=1&reg=0x51&val=0x80
       GC2145 用 0xFE 切换页，P0=0  P1=1  P3=3 */
    if (getParam(req, "reg", val, sizeof(val))) {
      int r = (int)strtol(val, NULL, 0);
      int v = 0;
      char vbuf[16];
      if (getParam(req, "val", vbuf, sizeof(vbuf))) v = (int)strtol(vbuf, NULL, 0);
      int page = 0;
      char pbuf[8];
      if (getParam(req, "page", pbuf, sizeof(pbuf))) page = atoi(pbuf);

      s->set_reg(s, 0xFE, 0xFF, page);        /* 切页 */
      int old = s->get_reg(s, r, 0xFF);
      int wr  = s->set_reg(s, r, 0xFF, v);
      int cur = s->get_reg(s, r, 0xFF);
      Serial.printf("[REG] page=%d reg=0x%02X  old=0x%02X -> want=0x%02X -> now=0x%02X  ret=%d\n",
                    page, r, old, v, cur, wr);
    }
  }

  /* 软件校色参数（8.8 定点：256 = 1.0 倍） */
  bool lutDirty = false;
  if (getParam(req, "rg", val, sizeof(val))) { g_rGain = (uint16_t)atoi(val); lutDirty = true; }
  if (getParam(req, "gg", val, sizeof(val))) { g_gGain = (uint16_t)atoi(val); lutDirty = true; }
  if (getParam(req, "bg", val, sizeof(val))) { g_bGain = (uint16_t)atoi(val); lutDirty = true; }
  if (getParam(req, "cf", val, sizeof(val))) g_colorFix = (atoi(val) != 0);
  if (lutDirty) rebuildLUT();

  int w = 0, h = 0;
  switch (g_fs) {
    case FRAMESIZE_QVGA: w = 320; h = 240; break;
    case FRAMESIZE_CIF:  w = 400; h = 296; break;
    case FRAMESIZE_HVGA: w = 480; h = 320; break;
    case FRAMESIZE_VGA:  w = 640; h = 480; break;
    case FRAMESIZE_SVGA: w = 800; h = 600; break;
    case FRAMESIZE_XGA:  w = 1024; h = 768; break;
    default: break;
  }

  uint32_t fps10 = 0;
  if (g_lastCostMs > 0) fps10 = 10000 / g_lastCostMs;

  char json[420];
  snprintf(json, sizeof(json),
           "{\"size\":\"%dx%d\",\"quality\":%d,\"compress_ms\":%u,\"fps_x10\":%u,"
           "\"frames\":%u,\"free_psram_kb\":%u,"
           "\"rg\":%u,\"gg\":%u,\"bg\":%u,\"cf\":%d,"
           "\"drv_awb\":%d,\"slv_addr\":\"0x%02X\"}\n",
           w, h, g_quality, g_lastCostMs, fps10,
           g_frameCount,
           psramFound() ? ESP.getFreePsram() / 1024 : 0,
           g_rGain, g_gGain, g_bGain, g_colorFix ? 1 : 0,
           s ? s->status.awb : -1,
           s ? s->slv_addr : 0);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* ---------- 播放页 ---------- */

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="theme-color" content="#111111">
<title>家里</title>
<style>
  * { box-sizing: border-box; }
  html,body { margin:0; height:100%; background:#0d0d0d; overflow:hidden;
              font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif; }
  #wrap { position:relative; width:100%; height:100%; display:flex;
          align-items:center; justify-content:center; }
  #v { max-width:100%; max-height:100%; display:block; }
  #hud { position:fixed; left:0; right:0; bottom:0; padding:10px 14px;
         font-size:12px; line-height:1.6; color:#c8c8c8;
         background:linear-gradient(transparent,rgba(0,0,0,.78));
         display:flex; justify-content:space-between; align-items:center; gap:8px; }
  #dot { display:inline-block; width:7px; height:7px; border-radius:50%;
         background:#4ade80; margin-right:6px; vertical-align:1px; }
  #dot.off { background:#ef4444; }
  .btn { border:1px solid #444; background:#1c1c1c; color:#ddd; border-radius:6px;
         padding:6px 10px; font-size:12px; margin-left:6px; white-space:nowrap; }
  .btn:active { background:#333; }
  #pause { position:absolute; inset:0; display:none; align-items:center;
           justify-content:center; color:#888; font-size:14px; }
</style>
</head>
<body>
<div id="wrap">
  <img id="v" alt="">
  <div id="pause">已暂停</div>
</div>
<div id="hud">
  <span><span id="dot"></span><span id="stat">连接中…</span></span>
  <span>
    <button class="btn" id="btnSize">400x296</button>
    <button class="btn" id="btn">暂停</button>
  </span>
</div>
<script>
(function () {
  var img = document.getElementById('v'),
      stat = document.getElementById('stat'),
      dot = document.getElementById('dot'),
      pauseLayer = document.getElementById('pause'),
      btn = document.getElementById('btn'),
      btnSize = document.getElementById('btnSize');

  var running = true, frames = 0, t0 = 0, fail = 0;
  var realFps = 0, realSize = '';
  var sizes  = ['qvga','cif','hvga','vga','svga'];
  var labels = ['320x240','400x296','480x320','640x480','800x600'];
  var si = 1;

  function next() {
    if (!running) return;
    img.src = '/snap?t=' + Date.now();
  }

  /* 每 2 秒问一次板子的真实帧率（页面拉取频率不代表画面更新频率） */
  setInterval(function () {
    fetch('/set', { credentials: 'include' })
      .then(function (r) { return r.json(); })
      .then(function (j) { realFps = j.fps_x10 / 10; realSize = j.size; })
      .catch(function () {});
  }, 2000);

  img.onload = function () {
    fail = 0; frames++;
    if (!t0) t0 = Date.now();
    var sec = (Date.now() - t0) / 1000;
    if (sec > 0.5) {
      dot.className = '';
      var pull = (frames / sec).toFixed(1);
      if (realFps > 0) {
        stat.textContent = realFps.toFixed(1) + ' fps · ' + realSize +
                           ' (拉取 ' + pull + ')';
      } else {
        stat.textContent = pull + ' fps · ' +
                           img.naturalWidth + ' x ' + img.naturalHeight;
      }
    }
    setTimeout(next, 15);
  };

  img.onerror = function () {
    fail++;
    dot.className = 'off';
    stat.textContent = fail > 4 ? '信号中断，重试中' : '重连中...';
    setTimeout(next, Math.min(600 + fail * 300, 3000));
  };

  btnSize.onclick = function () {
    si = (si + 1) % sizes.length;
    stat.textContent = '切换中…';
    running = false;
    fetch('/set?size=' + sizes[si], { credentials: 'include' })
      .then(function () { return fetch('/set'); })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        btnSize.textContent = j.size;
        running = true; frames = 0; t0 = 0; fail = 0;
        setTimeout(next, 400);
      })
      .catch(function () { running = true; next(); });
  };

  btn.onclick = function () {
    if (running) {
      running = false;
      pauseLayer.style.display = 'flex';
      btn.textContent = '继续';
      dot.className = 'off';
      stat.textContent = '已暂停';
    } else {
      running = true; frames = 0; t0 = 0; fail = 0;
      pauseLayer.style.display = 'none';
      btn.textContent = '暂停';
      next();
    }
  };

  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      running = false;
      btn.textContent = '继续';
      pauseLayer.style.display = 'flex';
      stat.textContent = '后台已暂停';
    }
  });

  next();
})();
</script>
</body>
</html>
)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  if (!checkAuth(req)) { sendAuthRequired(req); return ESP_OK; }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---------- 服务 ---------- */

static void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port      = 80;
  cfg.max_uri_handlers = 8;
  cfg.stack_size       = 12288;
  cfg.lru_purge_enable = true;

  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    Serial.println("[HTTP] 启动失败");
    return;
  }

  httpd_uri_t u1 = { .uri = "/",       .method = HTTP_GET, .handler = indexHandler,  .user_ctx = NULL };
  httpd_uri_t u2 = { .uri = "/snap",   .method = HTTP_GET, .handler = snapHandler,   .user_ctx = NULL };
  httpd_uri_t u3 = { .uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = NULL };
  httpd_uri_t u4 = { .uri = "/set",    .method = HTTP_GET, .handler = setHandler,    .user_ctx = NULL };

  httpd_register_uri_handler(g_server, &u1);
  httpd_register_uri_handler(g_server, &u2);
  httpd_register_uri_handler(g_server, &u3);
  httpd_register_uri_handler(g_server, &u4);

  Serial.println("[HTTP] 端口 80");
}

static void connectWiFi() {
  Serial.printf("[WiFi] 连接 \"%s\"\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("esp32-cam");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] 已连接 http://");
    Serial.print(WiFi.localIP());
    Serial.printf("/  信号 %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("[WiFi] 超时");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== ESP32-S3-Cam (GC2145) 定版 ===");

  buildAuthHeader();
  rebuildLUT();

  /* JPEG 缓冲放 PSRAM，SVGA 也就几十 KB */
  g_jpgBuf = (uint8_t *)ps_malloc(256 * 1024);
  if (!g_jpgBuf) {
    Serial.println("PSRAM 分配失败！");
    delay(5000);
    ESP.restart();
  }
  g_bufLock = xSemaphoreCreateMutex();

  if (!startCamera(g_fs, g_quality)) {
    Serial.println("摄像头失败，10 秒后重启");
    delay(10000);
    ESP.restart();
  }

  /* 采集任务钉在 Core 1，HTTP 服务跑 Core 0，互不打扰 */
  xTaskCreatePinnedToCore(camTask, "camTask", 16384, NULL, 4, NULL, 1);

  connectWiFi();
  startServer();

  Serial.println("[SYS] 就绪");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 15000) {
    last = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] 重连");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    } else {
      Serial.printf("[状态] 信号 %d dBm  压缩 %u ms  已出 %u 帧  堆 %u KB\n",
                    WiFi.RSSI(), g_lastCostMs, g_frameCount, ESP.getFreeHeap() / 1024);
    }
    if (esp_timer_get_time() > 24LL * 3600LL * 1000000LL) ESP.restart();
  }
  delay(200);
}
