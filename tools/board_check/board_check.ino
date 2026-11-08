/*
 * ESP32-S3 硬件自检
 * ------------------------------------------------------------
 * 作用：烧进去以后打开串口监视器，把板子的真实配置全打印出来。
 *      先跑这个，确认 Flash / PSRAM / 芯片型号都对，再上摄像头固件。
 *
 * 烧录设置（实测这台板子，选错会黑屏/无输出/无限重启）：
 *   Board            : ESP32S3 Dev Module
 *   Flash Size       : 8MB (64Mb)     <-- 实测 8MB，不是 16MB
 *   PSRAM            : OPI PSRAM
 *   Partition Scheme : Huge APP (3MB No OTA/1MB SPIFFS)
 *   USB CDC On Boot  : Disabled       <-- 板子走 CH340，必须 Disabled
 *
 * 为什么 CDCOnBoot 必须 Disabled：
 *   开了以后 Arduino 会把 Serial 重定向到 ESP32-S3 的原生 USB，
 *   而这块板子的数据是走 CH340 -> UART0 出来的，原生 USB 没引出，
 *   结果就是串口监视器一片空白，让人误以为板子坏了。
 *
 * 串口监视器波特率：115200
 */

#include <WiFi.h>
#include "esp_system.h"
#include "esp_chip_info.h"

static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "上电";
    case ESP_RST_EXT:      return "外部复位脚";
    case ESP_RST_SW:       return "软件重启";
    case ESP_RST_PANIC:    return "崩溃 panic";
    case ESP_RST_INT_WDT:  return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT:      return "其他看门狗";
    case ESP_RST_DEEPSLEEP:return "深度睡眠唤醒";
    case ESP_RST_BROWNOUT: return "电压不足 brownout";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "未知";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);                       // 等 USB CDC 枚举完，否则开头几行会丢

  Serial.println();
  Serial.println("================================");
  Serial.println("     ESP32-S3 硬件自检");
  Serial.println("================================");

  /* ---------- 芯片 ---------- */
  esp_chip_info_t info;
  esp_chip_info(&info);

  Serial.println("[ 芯片 ]");
  Serial.printf("  型号        : %s\n", ESP.getChipModel());
  Serial.printf("  核心数      : %d  (%s)\n", info.cores,
                (info.features & CHIP_FEATURE_EMB_PSRAM) ? "内置 PSRAM" : "无内置 PSRAM");
  Serial.printf("  修订版本    : v%d.%d\n", info.revision / 100, info.revision % 100);
  Serial.printf("  CPU 频率    : %u MHz\n", getCpuFrequencyMhz());

  /* ---------- 存储 ---------- */
  Serial.println("[ 存储 ]");
  Serial.printf("  Flash       : %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("  可用堆      : %u KB\n", ESP.getFreeHeap() / 1024);

  if (psramFound()) {
    Serial.printf("  PSRAM       : 有  %u MB   (空闲 %u KB)\n",
                  ESP.getPsramSize() / (1024 * 1024),
                  ESP.getFreePsram() / 1024);
    Serial.println("                ^ 这个是关键，没有它摄像头只能跑 QVGA");
  } else {
    Serial.println("  PSRAM       : 没检测到  <<<<<< 注意！");
    Serial.println("                多半是板子设置里 PSRAM 选成了 Disabled/QSPI");
    Serial.println("                N16R8 这个型号必须选 OPI PSRAM");
  }

  /* ---------- 运行时 ---------- */
  Serial.println("[ 运行时 ]");
  Serial.printf("  Arduino 核心: %s\n", ESP.getSdkVersion());
  Serial.printf("  编译时间    : %s %s\n", __DATE__, __TIME__);

  /* ---------- WiFi ---------- */
  Serial.println("[ WiFi ]");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  Serial.printf("  扫描到 %d 个 2.4G 热点\n", n);
  for (int i = 0; i < n && i < 8; i++) {
    Serial.printf("    %2d. %-28s  %d dBm  %s\n",
                  i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "开放" : "加密");
  }
  if (n > 8) Serial.printf("    ... 还有 %d 个\n", n - 8);

  /* ---------- 结论 ---------- */
  Serial.println("================================");
  bool ok = psramFound() && ESP.getFlashChipSize() >= 8 * 1024 * 1024;
  if (ok) {
    Serial.println("  结果：配置正确，可以上摄像头固件了");
  } else {
    Serial.println("  结果：配置有问题，先别烧摄像头固件");
    if (!psramFound()) Serial.println("        -> 去 Tools > PSRAM 选 OPI PSRAM");
    if (ESP.getFlashChipSize() < 8 * 1024 * 1024)
      Serial.println("        -> 去 Tools > Flash Size 选对型号");
  }
  Serial.println("================================");

  Serial.println("\n自检完成，5 秒后重新扫描...\n");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("[心跳] 运行 %lu 秒   堆 %u KB   PSRAM %u KB\n",
                  millis() / 1000,
                  ESP.getFreeHeap() / 1024,
                  psramFound() ? ESP.getFreePsram() / 1024 : 0);
  }
  delay(200);
}
