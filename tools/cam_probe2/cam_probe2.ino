/*
 * ESP32-S3 摄像头探测 v2
 * ------------------------------------------------------------
 * v1 的现象：JPEG 格式报「不支持」。
 * 这一步改用 RGB565 格式初始化 —— RGB565 几乎所有 sensor 都支持，
 * 能初始化成功就说明硬件链路是通的，同时可以读到传感器 ID 确认型号。
 *
 * 结论会落到三种情况：
 *   A. RGB565 成功 + PID=0x26  -> 是 OV2640，JPEG 报错另有原因
 *   B. RGB565 成功 + PID≠0x26  -> 卖家换模组了，不是 OV2640
 *   C. RGB565 也失败           -> 引脚或排线问题
 */

#include "esp_camera.h"
#include "esp_timer.h"

#define P_PWDN   (-1)
#define P_RESET  (-1)
#define P_XCLK   15
#define P_SIOD    4
#define P_SIOC    5
#define P_D7     16
#define P_D6     17
#define P_D5     18
#define P_D4     12
#define P_D3     10
#define P_D2      8
#define P_D1      9
#define P_D0     11
#define P_VSYNC   6
#define P_HREF    7
#define P_PCLK   13

static const char *sensorName(uint8_t pid) {
  switch (pid) {
    case 0x26: return "OV2640 (200万, 支持硬件JPEG)";
    case 0x56: return "OV5640 (500万, 支持硬件JPEG)";
    case 0x36: return "OV3660 (300万, 支持硬件JPEG)";
    case 0x21: return "OV7670 (30万, 只支持YUV/RGB)";
    case 0x77: return "OV7725 (30万, 只支持YUV/RGB)";
    case 0x00: return "GC0308 / GC 系列 (30万, 不支持JPEG)";
    default:   return "未知型号";
  }
}

static bool tryFormat(const char *label, pixformat_t fmt, framesize_t fs) {
  Serial.printf("\n---- %s ----\n", label);

  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_pwdn = P_PWDN;  c.pin_reset = P_RESET;
  c.pin_xclk = P_XCLK;  c.pin_sccb_sda = P_SIOD; c.pin_sccb_scl = P_SIOC;
  c.pin_d0 = P_D0; c.pin_d1 = P_D1; c.pin_d2 = P_D2; c.pin_d3 = P_D3;
  c.pin_d4 = P_D4; c.pin_d5 = P_D5; c.pin_d6 = P_D6; c.pin_d7 = P_D7;
  c.pin_vsync = P_VSYNC; c.pin_href = P_HREF; c.pin_pclk = P_PCLK;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = fmt;
  c.frame_size   = fs;
  c.jpeg_quality = 12;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("  × 初始化失败  err=0x%X\n", err);
    return false;
  }

  Serial.println("  √ 初始化成功，硬件链路通");

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    Serial.printf("  PID = 0x%02X   VER = 0x%02X\n", s->id.PID, s->id.VER);
    Serial.printf("  型号解读: %s\n", sensorName(s->id.PID));
    Serial.println("  能力位:");
    Serial.printf("    JPEG      : %s\n", (s->pixformat & PIXFORMAT_JPEG) ? "支持" : "不支持");
    Serial.printf("    RGB565    : %s\n", (s->pixformat & PIXFORMAT_RGB565) ? "支持" : "不支持");
    Serial.printf("    YUV422    : %s\n", (s->pixformat & PIXFORMAT_YUV422) ? "支持" : "不支持");
    Serial.printf("    GRAYSCALE : %s\n", (s->pixformat & PIXFORMAT_GRAYSCALE) ? "支持" : "不支持");
  }

  /* 抓 3 帧看数据是否真的出来了 */
  Serial.println("  抓 3 帧:");
  int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < 3; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.printf("    第%d帧 失败\n", i + 1); continue; }
    Serial.printf("    第%d帧  %dx%d  %u 字节  fmt=%d\n",
                  i + 1, fb->width, fb->height, fb->len, fb->format);
    esp_camera_fb_return(fb);
  }
  int64_t dt = (esp_timer_get_time() - t0) / 1000;
  Serial.printf("  3 帧耗时 %lld ms，平均 %lld ms/帧\n", dt, dt / 3);

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("========================================");
  Serial.println("   ESP32-S3 摄像头探测 v2");
  Serial.println("========================================");
  Serial.printf("PSRAM: %u MB\n", psramFound() ? ESP.getPsramSize() / (1024 * 1024) : 0);

  /* 第一优先：RGB565，几乎所有 sensor 都支持 */
  bool ok = tryFormat("RGB565 / QVGA", PIXFORMAT_RGB565, FRAMESIZE_QVGA);

  if (!ok) {
    esp_camera_deinit();
    delay(300);
    /* 再试 YUV422，有些 sensor 只吃这个 */
    ok = tryFormat("YUV422 / QVGA", PIXFORMAT_YUV422, FRAMESIZE_QVGA);
  }

  if (!ok) {
    esp_camera_deinit();
    delay(300);
    /* 试灰度，最保守 */
    ok = tryFormat("GRAYSCALE / QVGA", PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA);
  }

  Serial.println("\n========================================");
  if (ok) {
    Serial.println("  结论：硬件链路正常，sensor 已识别");
    Serial.println("  请看上面的 PID 判断型号，再决定后面怎么走");
  } else {
    Serial.println("  结论：三种格式全失败");
    Serial.println("  -> SCCB 完全没通，是引脚/排线问题");
  }
  Serial.println("========================================");
  Serial.println("\n探测结束，程序停住。");
}

void loop() {
  delay(1000);
}
