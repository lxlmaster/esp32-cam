/*
 * 画面验证：抓一帧，压成 JPEG，用 base64 从串口吐出来
 * ------------------------------------------------------------
 * 目的：确认 GC2145 出来的画面是不是正常的（怀疑全黑）。
 * 用法：串口抓到 ---BEGIN--- 和 ---END--- 之间的 base64，解码成 .jpg 就能看。
 */

#include "esp_camera.h"
#include "img_converters.h"
#include "mbedtls/base64.h"

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

static unsigned char b64buf[30000];

static void dumpOne(int idx, int quality) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { Serial.printf("#%d 抓帧失败\n", idx); return; }

  uint8_t *jpg = NULL;
  size_t len = 0;
  if (!frame2jpg(fb, quality, &jpg, &len)) {
    Serial.printf("#%d 压缩失败\n", idx);
    esp_camera_fb_return(fb);
    return;
  }
  esp_camera_fb_return(fb);

  /* 统计 RGB 数据特征，判断是不是全黑/全白 */
  size_t olen = 0;
  if (mbedtls_base64_encode(b64buf, sizeof(b64buf), &olen, jpg, len) != 0) {
    Serial.printf("#%d base64 失败\n", idx);
    free(jpg);
    return;
  }

  Serial.printf("\n===BEGIN_%d len=%u b64=%u===\n", idx, (unsigned)len, (unsigned)olen);
  /* 每行 100 字符输出，避免串口缓冲溢出 */
  for (size_t i = 0; i < olen; i += 100) {
    size_t n = (olen - i < 100) ? (olen - i) : 100;
    Serial.write(b64buf + i, n);
    Serial.write('\n');
    delay(2);
  }
  Serial.printf("===END_%d===\n", idx);

  free(jpg);
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("### 画面抓取程序启动 ###");

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
  c.pixel_format = PIXFORMAT_RGB565;
  c.frame_size   = FRAMESIZE_QVGA;
  c.jpeg_quality = 12;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&c) != ESP_OK) {
    Serial.println("!!! 摄像头初始化失败 !!!");
    return;
  }
  Serial.println("摄像头就绪");

  /* 先扔掉几帧，让自动曝光稳定 */
  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(120);
  }
  Serial.println("预热完成，开始输出");

  dumpOne(1, 12);
  delay(500);
  dumpOne(2, 12);

  Serial.println("\n### 输出完毕 ###");
}

void loop() {
  delay(2000);
}
