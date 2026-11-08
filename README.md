# ESP32-S3 家庭摄像头

用一块 ESP32-S3-CAM（¥44）自建的联网摄像头 —— 手机随时打开看一眼家里。不做录像回放、不做夜视，只要实时画面。

> 关键前提：这块板子的摄像头是 **GC2145**，**没有硬件 JPEG 编码器**，全靠 ESP32 用 CPU 软件压缩。这决定了整个架构，详见下文。

---

## 硬件

| 部件 | 型号 | 价格 |
|---|---|---|
| 主控板 | ESP32-S3-CAM（幻尔 Hiwonder 套件） | ¥44 |
| 摄像头 | **GC2145**（板载，非 OV2640） | 板载 |
| 内存 | 8MB Flash + 8MB PSRAM（实测，卖家标 N16R8 有误） | 板载 |
| 串口芯片 | CH340（需装驱动） | 板载 |
| 电源 | 5V/2A USB | ¥15 |

完整清单见 [docs/物料清单.md](docs/物料清单.md)。

### 实测硬件参数

```
芯片      ESP32-S3 (QFN56) rev v0.2
内核      Dual Core + LP Core @ 240MHz
Flash     8MB
PSRAM     8MB（封装内置，OPI）
MAC       e0:72:a1:d0:00:8c
传感器    GC2145  PID=0x2145  SCCB 地址 0x3C
```

---

## 核心结论：这块板子的摄像头是 GC2145，不是 OV2640

这是整个项目最关键的一条，直接决定了架构：

| | OV2640 | GC2145（本项目） |
|---|---|---|
| 硬件 JPEG 编码 | ✅ 有 | ❌ **没有** |
| 输出格式 | JPEG 直出 | 只能 RGB565 / YUV |
| 一帧 QVGA 数据量 | ~15KB | **150KB** |
| ESP32 要做的事 | 只转发 | 还得用 CPU 压缩 |

所以本固件的流程是：

```
RGB565 原始帧 → 软件色彩处理 → frame2jpg() 压成 JPEG → HTTP 发送
```

**代价**：压缩耗时与像素数成正比，帧率上限被 CPU 算力锁死。

---

## 性能实测

均为软件 JPEG 压缩（`frame2jpg`），ESP32-S3 @ 240MHz：

| 分辨率 | 压缩耗时 | 实际帧率 | 评价 |
|---|---|---|---|
| 320×240 | 80ms | 12.5 fps | 糊 |
| **400×296** | **100ms** | **10.0 fps** | ✅ **默认，平衡点** |
| 480×320 | 160ms | 6.2 fps | 略卡 |
| 640×480 | 321ms | 3.1 fps | 幻灯片 |

抓图请求响应 61-91ms。

### 曾走过的弯路：串行 vs 流水线

| 架构 | 480×320 帧率 |
|---|---|
| 抓帧→压缩→发送 全在 HTTP handler 里串行 | ~4 fps |
| **独立采集任务持续压缩 + HTTP 只取成品** | **7.6 fps** |

流水线带来接近一倍的提升，是本项目最重要的优化。

> ⚠️ **注意**：流水线架构下，浏览器拉取 `/snap` 的频率**不等于**画面更新频率 —— 因为每次请求都立刻返回缓存帧。页面显示的 fps 必须用 `1000 / compress_ms` 计算，否则会看到「10fps 很流畅」的假象（实际只有 3fps）。

---

## 快速开始

### 1. 环境

```bash
# 板包索引（官方源在国内超时，必须用乐鑫中国 CDN）
arduino-cli config set board_manager.additional_urls \
  https://dl.espressif.cn/dl/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.12
```

CH340 驱动：Windows 需手动装 WCH 官方驱动（可从 `himalayanelixir/arduino-usb-drivers` 取 WHQL 签名版）。

### 2. 改配置

编辑 `firmware/esp32_cam_final/esp32_cam_final.ino` 第 42-45 行：

```c
const char *WIFI_SSID = "你的WiFi名";        // 必须是 2.4G，5G 连不上
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char *AUTH_USER = "cam";
const char *AUTH_PASS = "YOUR_CAM_PASSWORD";  // 访问网页的密码，用 ASCII，别用中文
```

### 3. 编译烧录

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=huge_app,CDCOnBoot=default,UploadSpeed=921600" \
  --upload -p COM3 firmware/esp32_cam_final
```

### 4. 访问

串口会打印 IP，浏览器打开 `http://<IP>/`，输入账号密码。

---

## 板子设置（选错会黑屏/无输出/起不来）

| 选项 | 值 | 说明 |
|---|---|---|
| Board | ESP32S3 Dev Module | |
| Flash Size | **8MB** | 实测 8MB，选 16MB 分区表对不上 |
| PSRAM | **OPI PSRAM** | 必选，否则摄像头降级 |
| Partition Scheme | Huge APP (3MB No OTA) | |
| **USB CDC On Boot** | **Disabled** | ⚠️ 板子走 CH340→UART0，开了串口一片空白 |
| Upload Speed | 921600 | |

---

## HTTP 接口

全部需要 HTTP Basic Auth。

| 接口 | 说明 |
|---|---|
| `GET /` | 播放页（拉模式，每帧请求一次 `/snap`） |
| `GET /snap` | 单张 JPEG |
| `GET /stream` | MJPEG 流（给 VLC 用，iOS Safari 不支持） |
| `GET /set` | 查看当前参数（返回 JSON） |
| `GET /set?size=qvga\|cif\|hvga\|vga\|svga` | 切分辨率 |
| `GET /set?q=10` | JPEG 质量 0-63 |
| `GET /set?rg=256&gg=256&bg=256` | 软件色彩增益（8.8 定点，256=1.0倍） |
| `GET /set?cf=0` | 关/开色彩校正 |
| `GET /set?vflip=1` / `?hmirror=1` | 上下翻转 / 左右镜像 |
| `GET /set?reg=0x51&val=0x80&page=1` | 直接写传感器寄存器 |

---

## 目录结构

```
.
├── firmware/
│   └── esp32_cam_final/      # 生产用固件
├── tools/                     # 调试诊断工具
│   ├── board_check/           # 硬件自检：芯片/Flash/PSRAM/WiFi 扫描
│   ├── cam_probe2/            # 摄像头探测：RGB565 绕过 JPEG 检查读 PID
│   ├── cam_jpeg_test/         # 软件压缩性能测试
│   └── cam_dump/              # 串口 base64 dump 画面（无需 WiFi 就能看图）
└── docs/
    ├── 物料清单.md
    ├── 采购清单.md
    └── 硬件实测与踩坑.md       # 完整踩坑记录，建议先看
```

### 工具用法

**不知道板子引脚/传感器型号时**，先跑 `cam_probe2`：它用 RGB565 初始化（几乎所有 sensor 都支持），能绕开「不支持 JPEG」的报错，直接把 PID 读出来。

```bash
arduino-cli compile --fqbn "..." --upload -p COM3 tools/cam_probe2
```

**没有 WiFi 也想看画面**，用 `cam_dump`：把 JPEG 用 base64 从串口吐出来，电脑端解码成图片。

---

## 已知限制

| 限制 | 原因 | 解法 |
|---|---|---|
| 帧率上不去 | GC2145 无硬件 JPEG，靠 CPU 压缩 | 换 OV2640 模组（¥20），640×480 可到 12fps |
| 画面偏色只能软件修 | 驱动的 `set_whitebal/set_saturation` 全是空函数 | 用 `/set?rg=` 软件校色，或 `/set?reg=` 直写寄存器 |
| 暗光噪点大 | 无夜视/红外补光 | 加补光灯 |
| 光波（横向条纹） | LED 频闪 × rolling shutter | 换白炽灯，或让 AEC 锁曝光 |
| 只能局域网 | 光猫无公网映射、IPv6 防火墙关不掉 | 节点小宝 P2P 打洞（实测 7.8fps 可用） |

更多细节见 [docs/硬件实测与踩坑.md](docs/硬件实测与踩坑.md)。

---

## 后续计划

- [ ] 镜头调焦（物理操作，模组是螺纹的）
- [ ] 第二路摄像头（手上有第二块同型号板）
- [ ] 外网访问（节点小宝 P2P）
- [ ] 可选：换 OV2640 模组彻底解决帧率

---

## License

MIT
