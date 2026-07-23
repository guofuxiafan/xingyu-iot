# ESP32-P4 摄像头采集与 WebSocket 推流

本项目基于 ESP-IDF 与乐鑫 `simple_video_server` 示例改造，面向 Waveshare ESP32-P4-WIFI6-DEV-KIT。它可采集 MIPI-CSI 和 USB UVC 摄像头画面，将帧编码为 JPEG 后推送到局域网 PC；在常规模式下还提供用于查看设备状态和调节 JPEG 质量的网页界面。

## 当前默认配置

- 目标芯片：ESP32-P4；适配 Waveshare ESP32-P4-WIFI6-DEV-KIT。
- 网络：板载 ESP32-C6 通过 ESP-HOSTED/SDIO 提供 Wi-Fi；也可按 ESP-IDF 配置改用以太网。
- 摄像头：默认启用 OV5647 MIPI-CSI 与一台 USB UVC 摄像头；可在 `menuconfig` 中调整传感器、分辨率与 USB 摄像头数量。
- 默认运行模式：USB + CSI 网络验证模式。CSI 画面为 `cam0`，USB UVC 画面为 `cam1`，二者通过 WebSocket 推送到 PC；此模式不启动普通网页 HTTP 服务。
- 图像传输：设备读取 NVS 中保存的 PC IP，并连接 `ws://<PC_IP>:8765`。每个二进制消息为：`8 字节小端时间戳 + 1 字节摄像头 ID + 2 字节大端 JPEG 长度 + JPEG 数据`。

## 功能概览

- 基于 ESP Video/V4L2 的 CSI 与 USB UVC 采集；非 JPEG 输入会编码为 JPEG。
- USB 摄像头初始化失败或运行时掉线时自动重试。
- WebSocket 发送队列与固定帧池，避免采集任务被网络发送阻塞。
- 首次使用或长按 BOOT 键时启动 AP 配网页；保存 Wi-Fi 凭据和 PC IP 到 NVS。
- 常规模式提供 mDNS（默认 `esp-web.local`）、设备状态 API 和 JPEG 质量设置 API。
- 可选 USB 最小验证、USB 经 HUB、USB + CSI、双 USB 等调试模式。
- 每 60 秒输出一次堆内存与 PSRAM 诊断信息（仅特定网络验证模式）。

## 快速开始

### 1. 准备环境

- ESP-IDF 5.4 或更高版本（建议与当前依赖兼容的版本）。
- Python 与 ESP-IDF 工具链。
- 若修改网页前端：Node.js 20+、pnpm 和可用的 `gzip`。
- Waveshare ESP32-P4-WIFI6-DEV-KIT、OV5647（可选）和 USB UVC 摄像头。

### 2. 配置并构建固件

在 ESP-IDF 已导出的终端中执行：

```bash
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

在 `menuconfig` 中重点确认：

- 摄像头接口和型号：`Example Video Initialization Configuration` 与 `Espressif Camera Sensors Configurations`。
- 板载 C6 的 Wi-Fi Remote/ESP-HOSTED 配置，或改为以太网。
- `Example Configuration` 中的 USB/CSI 验证模式、缓冲区数量、USB UVC URB 数和 JPEG 质量。
- `Provisioning Configuration` 中的配网 AP 名称、重试次数和长按 BOOT 的时长。

项目的 `sdkconfig.defaults.esp32p4` 已包含 16 MB Flash、PSRAM、C6 SDIO、OV5647 和 USB UVC 的默认设置。Waveshare 板级适配细节见 [PORTING_WAVESHARE.md](PORTING_WAVESHARE.md)。

### 3. 首次配网和接收端

设备没有保存 Wi-Fi 凭据时会建立 AP 配网热点。连接热点后，在浏览器中填写 Wi-Fi SSID、密码及接收 PC 的 IPv4 地址；设备会保存设置并连接路由器。PC 接收服务需监听 8765 端口并按上述 WebSocket 二进制帧格式解析数据。

若网络连接失败，设备会在重试后重启并再次尝试；长按 BOOT 键可清除已保存的网络配置并重新进入配网模式。

## 运行模式

| 模式 | 用途 | 网络/HTTP |
| --- | --- | --- |
| USB 最小验证 | 单 USB UVC 摄像头，定位 USB 等时传输问题 | 不启用 Wi-Fi、WebSocket、HTTP、mDNS 和配网 |
| USB 网络验证 | 单 USB UVC 摄像头向 PC 推流 | Wi-Fi + WebSocket，无普通 HTTP |
| USB HUB 验证 | 单摄像头经 Waveshare CH334 HUB 的 HOST2–HOST4 端口接入 | 同 USB 网络验证 |
| USB + CSI 网络验证 | `cam0` CSI 与 `cam1` USB 同时推流 | 当前默认；Wi-Fi + WebSocket，无普通 HTTP |
| 双 USB 网络验证 | 两路 USB UVC 摄像头推流 | Wi-Fi + WebSocket，无普通 HTTP |
| 常规模式 | 摄像头采集、网页管理及可选 WebSocket 推流 | 启动 HTTP/mDNS；配置了 PC IP 时同时推流 |

模式开关位于 `idf.py menuconfig` 的 **Example Configuration**。USB 摄像头接入不同物理端口时，应同时核对板卡跳线与 USB/HUB 相关配置。

## 常规模式的网页与 API

常规模式下，设备在端口 80 提供网页和 API；默认可通过 `http://esp-web.local` 或设备 IP 访问。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 返回内嵌的 Vue 网页资源 |
| GET | `/api/get_camera_info` | 返回已激活摄像头的编号、类型、分辨率、帧率和 JPEG 质量能力 |
| POST | `/api/set_camera_config` | 设置 JPEG 质量，请求体示例：`{"index": 0, "jpeg_quality": 80}` |
| POST | `/api/set_wifi_config` | 仅 AP 配网页使用，保存 Wi-Fi 与 PC IP |

注意：当前默认的 USB + CSI 网络验证模式不会启动这些 HTTP 接口；需要网页管理时请关闭 `USB Wi-Fi/WebSocket validation mode`，使用常规模式。

## 网页前端

前端位于 `frontend/`，使用 Vue 3、TypeScript、Vite 与 Vuetify。修改前端后重新生成压缩资源，再构建固件：

```bash
cd frontend
pnpm install
pnpm compress
cd ..
idf.py build
```

`pnpm compress` 会构建前端，并将结果压缩到 `frontend/gzipped/`；这些文件会由 `main/CMakeLists.txt` 嵌入固件。

## 目录说明

| 路径 | 作用 |
| --- | --- |
| `main/` | 固件入口、摄像头采集、Wi-Fi 配网、WebSocket 推流、HTTP 与诊断模块 |
| `main/camera_source.*` | V4L2 采集、JPEG 处理、缓冲区与掉线恢复相关逻辑 |
| `main/ws_streamer.*` | WebSocket 客户端、帧队列和发送任务 |
| `main/provisioning_manager.*` | AP 配网门户与凭据保存 |
| `frontend/` | Vue 网页源代码与压缩资源生成脚本 |
| `components/example_video_common/` | 摄像头初始化和编码公共组件 |
| `managed_components/` | ESP-IDF 组件管理器下载的依赖，不建议直接修改 |
| `PORTING_WAVESHARE.md` | Waveshare 板卡移植与引脚配置说明 |
| `partitions.csv` | Flash 分区表 |

## 排查建议

- 看不到 USB 摄像头：确认使用支持 UVC 的摄像头、供电充足、端口/跳线与所选验证模式匹配；串口日志会显示初始化重试。
- 无法连接 Wi-Fi：确认 C6/ESP-HOSTED 的 SDIO 配置和凭据；必要时长按 BOOT 后重新配网。
- PC 未收到画面：确认 PC IP 已在配网页保存、PC 防火墙允许 TCP 8765、接收服务已先启动，并且设备与 PC 在可互通的网络中。
- 网页无法打开：检查是否仍启用 USB 网络验证模式；该模式按设计不启动普通 HTTP 服务。
- 内存不足或丢帧：适当降低分辨率/帧率、JPEG 质量或 USB URB 与缓冲区数量，并观察串口的 heap/PSRAM 诊断日志。

## 许可与来源

本项目基于乐鑫 ESP-IDF 视频服务器示例进行修改；源文件保留各自的版权与许可证声明。
