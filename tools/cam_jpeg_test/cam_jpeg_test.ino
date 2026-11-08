/*
 * GC2145 软件 JPEG 编码性能测试
 * ------------------------------------------------------------
 * 背景：这块板配的是 GC2145，没有硬件 JPEG，只能拿 RGB565 原始帧
 *      再用 frame2jpg() 在 CPU 上压。这个程序专门测压缩速度。
 *
 * 判据：
 *   单帧压缩 < 80ms  -> 合计能跑 8fps 以上，够用，不用换模组
 *   单帧压缩 > 150ms -> 太慢，建议换 OV2640 模组
 */

#include "esp_camera.h"
#include "img_converters.h"
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

static bool initCam(framesize_t fs) {
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
  c.frame_size   = fs;
  c.jpeg_quality = 12;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

static void bench(const char *label, framesize_t fs, int quality, int rounds) {
  Serial.printf("\n======== %s (quality=%d) ========\n", label, quality);

  if (!initCam(fs)) {
    Serial.println("  初始化失败");
    return;
  }

  int64_t tGrab = 0, tJpeg = 0;
  size_t minJpg = 999999, maxJpg = 0, sumJpg = 0;
  int okCount = 0, total = 0;

  for (int i = 0; i < rounds; i++) {
    int64_t a = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    int64_t b = esp_timer_get_time();
    if (!fb) continue;

    uint8_t *jpg = NULL;
    size_t len = 0;
    bool ok = frame2jpg(fb, quality, &jpg, &len);
    int64_t c = esp_timer_get_time();

    total++;
    if (ok && jpg) {
      okCount++;
      tGrab += (b - a);
      tJpeg += (c - b);
      if (len < minJpg) minJpg = len;
      if (len > maxJpg) maxJpg = len;
      sumJpg += len;
      if (i < 3) {
        Serial.printf("  第%d帧 RGB=%u B -> JPEG=%u B  (抓%lldms 压%lldms)\n",
                      i + 1, fb->len, len, (b - a) / 1000, (c - b) / 1000);
      }
      free(jpg);
    } else {
      Serial.printf("  第%d帧 压缩失败\n", i + 1);
    }
    esp_camera_fb_return(fb);
    delay(20);
  }

  if (okCount > 0) {
    int64_t avgGrab = tGrab / okCount / 1000;
    int64_t avgJpeg = tJpeg / okCount / 1000;
    Serial.println("  ---------- 汇总 ----------");
    Serial.printf("  成功 %d/%d 帧\n", okCount, total);
    Serial.printf("  JPEG 大小: 最小 %u B / 最大 %u B / 平均 %u B\n",
                  (unsigned)minJpg, (unsigned)maxJpg, (unsigned)(sumJpg / okCount));
    Serial.printf("  平均抓帧 %lld ms，平均压缩 %lld ms，合计 %lld ms\n",
                  avgGrab, avgJpeg, avgGrab + avgJpeg);
    if (avgGrab + avgJpeg > 0) {
      float fps = 1000.0f / (float)(avgGrab + avgJpeg);
      Serial.printf("  ==> 理论极限 %.1f fps\n", fps);
      if (avgJpeg < 80)        Serial.println("  ==> 判定：压缩够快，不用换模组");
      else if (avgJpeg < 150)  Serial.println("  ==> 判定：勉强能用，帧率偏低");
      else                     Serial.println("  ==> 判定：太慢，建议换 OV2640");
    }
  }
  esp_camera_deinit();
  delay(500);
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("============================================");
  Serial.println("   GC2145 软件 JPEG 压缩性能测试");
  Serial.println("============================================");
  Serial.printf("PSRAM: %u MB   CPU: %u MHz\n",
                psramFound() ? ESP.getPsramSize() / (1024 * 1024) : 0,
                getCpuFrequencyMhz());

  /* 主测：QVGA，质量 12（和原固件一致） */
  bench("QVGA 320x240", FRAMESIZE_QVGA, 12, 10);

  /* 再测个更高画质，看代价 */
  bench("QVGA 320x240", FRAMESIZE_QVGA, 8, 8);

  /* 试 VGA，看能不能上 640x480 */
  bench("VGA 640x480", FRAMESIZE_VGA, 12, 6);

  Serial.println("\n============================================");
  Serial.println("  测试结束");
  Serial.println("============================================");
}

void loop() {
  delay(1000);
}
