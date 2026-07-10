# 项目移植指南 — Waveshare ESP32-P4-WIFI6-DEV-KIT

> 本文档分析了 `simple_video_server` 示例项目与 Waveshare ESP32-P4-WIFI6-DEV-KIT 开发板的差异，列出了所有需要自定义的配置项。

---

## 项目来源

这个项目是 Espressif 官方的 **simple_video_server** 示例（HTTP 摄像头视频流服务器），默认配置的是 **Espressif 自家的 ESP32-P4-Function-EV-Board V1.5** 开发板，不是 Waveshare 板。

好消息是：**摄像头 I2C 引脚（SCL=GPIO8, SDA=GPIO7）和 MIPI-CSI 接口恰好一致**，所以摄像头部分基本可以开箱即用。但以下几处必须自定义。

---

## 必须修改的地方（按优先级排序）

### 1. WiFi 网络配置（最关键）

ESP32-P4 本身没有 WiFi，Waveshare 板通过 **ESP32-C6 (SDIO)** 提供 WiFi6。项目已经依赖了 `esp_wifi_remote` 组件，但你需要：

| 修改项 | 文件 / 路径 | 当前值 | 需改成 |
|--------|------------|--------|--------|
| WiFi SSID | `menuconfig → Example Connection Configuration` | 空 | 你的 WiFi 名称 |
| WiFi 密码 | 同上 | 空 | 你的 WiFi 密码 |
| esp_wifi_remote slave target | `menuconfig → Wi-Fi Remote` | 未配置 | 选择 ESP32-C6 |

或者改用以太网（Waveshare 板有 RJ45 网口）：
- `menuconfig → Example Connection Configuration` → 选 Ethernet
- PHY 型号：查原理图（大概率是 IP101）
- PHY 地址：查原理图
- MDC=GPIO31, MDIO=GPIO52, PHY Rst=GPIO51

**相关文件**：
- `sdkconfig.defaults` 第 13 行：`CONFIG_EXAMPLE_CONNECT_WIFI=y`

---

### 2. 板卡选择 & DVP 引脚冲突

默认选的 "ESP32-P4-Function-EV-Board V1.5" 会同时启用 **MIPI-CSI + DVP**。DVP 占用的引脚（GPIO 2,3,4,5,6,20,21,22,23,32,33,36,37,38）可能和 Waveshare 板上的 SD 卡（39-44）、以太网（31,51,52）、音频（9-13）等冲突。

**推荐做法**：改选 "Customized Development Board"，只启用 MIPI-CSI，关闭 DVP：

```
menuconfig → Example Video Initialization Configuration → Select Target Development Board
  ( ) ESP32-P4-Function-EV-Board V1.5
  (X) Customized Development Board
```

然后：
```
Select and Set Camera Sensor Interface:
  [*] MIPI-CSI   ← 保留
  [ ] DVP        ← 关掉（除非你确实接了 DVP 摄像头）
  [ ] SPI        ← 关掉
```

MIPI-CSI 的 I2C 引脚保持默认即可（SCL=8, SDA=7 — 和 Waveshare 一致）。

**相关文件**：
- `sdkconfig.defaults.esp32p4` 第 9 行：`CONFIG_EXAMPLE_SELECT_ESP32P4_FUNCTION_EV_BOARD_V1_5=y`
- `components/example_video_common/Kconfig.projbuild` 第 3-46 行（板卡选择）

---

### 3. 摄像头传感器型号

项目默认选了两个传感器（有冲突）：
- `sdkconfig.defaults` → `CONFIG_CAMERA_OV5647=y`（OV5647 MIPI）
- `sdkconfig.defaults.esp32p4` → `CONFIG_CAMERA_SC2336=y`（SC2336 MIPI）

Waveshare 板官方支持 **OV5647** 摄像头。你需要确认你手头的摄像头模块型号，然后在 menuconfig 中只选一个：

```
menuconfig → Component config → Espressif Camera Sensors Configurations → Camera Sensor Configuration
```

**相关文件**：
- `sdkconfig.defaults` 第 14-16 行
- `sdkconfig.defaults.esp32p4` 第 1 行

---

### 4. Flash 大小（16MB）

Waveshare 板有 **16MB NOR Flash**，但项目没有显式设置 flash 大小。需要在 menuconfig 中确认：

```
menuconfig → Serial flasher config → Flash size
  → 选 16 MB
```

或者在 `sdkconfig.defaults.esp32p4` 中添加：
```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

---

### 5. 芯片版本配置

ESP32-P4 有不同版本（v0.x/v1.x 工程样品 vs v3.0+ 正式版）。Waveshare 官方文档提到需要根据芯片版本选择不同的构建配置。

烧录时如果出现 `requires chip revision in range [v3.1 - v3.99]` 之类的错误，需要：
```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;../../../config/esp32p4_rev_v3_1.defaults" set-target esp32p4 build
```

或直接在 menuconfig 中设置 `CONFIG_ESP32P4_REV_MIN_FULL`。

---

### 6. 分区表（可选）

当前分区表 `partitions.csv` 给 factory app 分了 1500K，对 16MB Flash 来说偏保守。如果你想放更大的固件（包含 Web UI 资源），可以增大：

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        3M,
```

**相关文件**：`partitions.csv`

---

## 不需要改的地方（已确认匹配）

| 配置项 | 项目默认值 | Waveshare 实际值 | 状态 |
|--------|-----------|-----------------|------|
| MIPI-CSI I2C SCL | GPIO8 | GPIO8 | 一致 |
| MIPI-CSI I2C SDA | GPIO7 | GPIO7 | 一致 |
| MIPI-CSI 通道数 | 2-lane | 2-lane | 一致 |
| PSRAM | SPIRAM 200M | 32MB 叠封 PSRAM | 兼容 |
| JPEG 编码器 | 硬件 JPEG | ESP32-P4 有 HW JPEG | 一致 |
| ISP Pipeline | 已启用 | ESP32-P4 有 ISP | 一致 |
| IDF 版本 | >=5.4 | Waveshare 推荐 v5.5.4 | 兼容 |
| esp_wifi_remote | 已在依赖中 | 需要（C6 SDIO WiFi） | 已就绪 |

---

## 快速操作清单

```bash
# 1. 设置目标芯片
cd D:\WHR\program\iot\camera
idf.py set-target esp32p4

# 2. 打开 menuconfig 进行以下修改：
idf.py menuconfig
```

在 menuconfig 中依次修改：

1. **`Example Video Initialization Configuration`**
   - Select Target Development Board → **Customized Development Board**
   - MIPI-CSI Camera Sensor → **启用**（SCL=8, SDA=7 保持默认）
   - DVP Camera Sensor → **关闭**
   - SPI Camera Sensor → **关闭**

2. **`Example Connection Configuration`**
   - WiFi SSID → 你的 WiFi 名
   - WiFi Password → 你的 WiFi 密码
   - 或切换为 Ethernet 模式

3. **`Wi-Fi Remote`**
   - Select slave target → **ESP32-C6**

4. **`Component config → Espressif Camera Sensors Configurations`**
   - 只勾选你实际使用的摄像头型号（OV5647 或 SC2336）

5. **`Serial flasher config`**
   - Flash size → **16 MB**

6. **保存退出，然后编译烧录：**
```bash
idf.py build
idf.py -p COMx flash monitor
```

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Waveshare 原理图 | [ESP32-P4-WIFI6-DEV-KIT-datasheet.pdf](https://www.waveshare.net/w/upload/3/39/ESP32-P4-WIFI6-DEV-KIT-datasheet.pdf) |
| Waveshare 官方示例仓库 | [github.com/waveshareteam/ESP32-P4-Platform](https://github.com/waveshareteam/ESP32-P4-Platform) |
| IDF 开发环境搭建 | [docs.waveshare.net/.../Development-Environment-Setup-IDF](https://docs.waveshare.net/ESP32-P4-WIFI6-DEV-KIT/Development-Environment-Setup-IDF) |
| 项目内引脚表 | `components/example_video_common/README.md` |
| Waveshare 硬件手册 | [docs.waveshare.net/ESP32-P4-WIFI6-DEV-KIT/Resources-And-Documents](https://docs.waveshare.net/ESP32-P4-WIFI6-DEV-KIT/Resources-And-Documents) |

---

> **建议**：烧录前先用 Waveshare 官方的 `00_board_check` 示例验证板子、串口、PSRAM 正常，再回来编译这个摄像头项目。
