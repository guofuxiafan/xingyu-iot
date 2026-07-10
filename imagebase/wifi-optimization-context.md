# WiFi 带宽优化 — 视频流降带宽方案

> 项目：ESP32-P4 + OV5647 摄像头视频流服务器
> 路径：`D:\WHR\program\iot\camera`
> 日期：2026-07-07

---

## 一、背景

ESP32-P4 采集 OV5647 摄像头 800×640 @ 50fps 视频，通过外挂 C6 从机（SDIO WiFi）以 MJPEG 流传输到浏览器。WiFi 实际吞吐约 5-10 Mbps，原始流带宽约 12-20 Mbps，导致视频卡顿。

## 二、最终方案

放弃硬件裁剪（驱动不支持），只保留两项应用层优化：

| 改动 | 文件 | 说明 |
|------|------|------|
| 帧率限流 | `main/simple_video_server_example.c:457` | `vTaskDelay(pdMS_TO_TICKS(66))` 将流帧率从 50fps 压到约 15fps |
| JPEG 质量降低 | `sdkconfig:774` | `CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY=40`（原 50） |

**带宽估算：** 800×640 @ 15fps JPEG q40 → 约 3-6 Mbps，在 C6 SDIO WiFi 的 5-10 Mbps 范围内。

## 三、代码改动明细

### 3.1 `main/simple_video_server_example.c`

```c
// 第 544 行：宏名修正（预防性，crop 已禁用所以不会进 #if 块）
selection.r.left = CONFIG_EXAMPLE_MIPI_CSI_CROP_TOP_LEFT_H;

// 第 546 行
selection.r.top  = CONFIG_EXAMPLE_MIPI_CSI_CROP_TOP_LEFT_V;

// 第 456-457 行：帧率限流（核心改动）
/* Frame rate throttle: limit to ~15fps to reduce WiFi bandwidth */
vTaskDelay(pdMS_TO_TICKS(66));
```

`vTaskDelay` 位于 `image_stream_handler()` 函数的 `while(1)` 循环末尾，在 `VIDIOC_QBUF` 之后。每帧延迟 66ms，实际输出帧率约 1/(0.066+send_time) ≈ 12-15fps。

### 3.2 `sdkconfig`

```
# 第 774 行
CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY=40

# 第 778 行：裁剪已回退为禁用
# CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CROP is not set
```

## 四、裁剪失败的根因

日志：
```
E (14875) esp_video: video->ops->set_selection=106
E (14875) example: failed to set selection
```

**根因：** `managed_components/espressif__esp_video` 驱动未实现 `VIDIOC_S_SELECTION` ioctl，返回 `errno 106 (ENOSYS)`。OV5647 sensor subdev 不支持 selection 操作，ISP 硬件裁剪无法使用。

**处置：** 回退 sdkconfig 中 crop 相关配置，仅靠帧率限流 + JPEG 质量降低。

## 五、编译

```powershell
# 设置 PATH（Windows PowerShell）
cmd /c "set PATH=C:\Espressif\tools\cmake\3.30.5\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\xtulip-esp-elf\esp-14.2.0_20260121\xtulip-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Users\VV\.espressif\python_env\idf5.5_py3.12_env\Scripts;%PATH% && ninja -C D:\WHR\program\iot\camera\build"
```

**输出：** 1156/1156 targets，0 errors
**固件：** `D:\WHR\program\iot\camera\build\simple_video_server.bin` (~1.25MB)

## 六、项目结构速览

```
camera/
├── main/                          ← 用户代码
│   ├── simple_video_server_example.c   ← 主程序（我们改了这里）
│   └── Kconfig.projbuild               ← 配置项定义
├── managed_components/            ← 第三方依赖（不改！）
│   ├── espressif__esp_video/      ← 视频驱动
│   ├── espressif__esp_wifi_remote/← WiFi 遥控（C6 从机通信）
│   └── espressif__esp_hosted/     ← C6 SDIO 协议
├── sdkconfig                      ← 相当于 CubeMX .ioc 文件
├── build/                         ← 编译输出
└── frontend/                      ← 网页前端（Vue.js）
```

**双芯片架构：**
```
ESP32-P4 (主 CPU, Xtensa)  ←──SDIO──→  ESP32-C6 (WiFi 从机, RISC-V)
  摄像头采集、HTTP 服务器                WiFi 连接、网络协议栈
```

## 七、视频流抓帧工具

脚本位置：`imagebase/capture_frames.py`

```powershell
# 安装依赖
pip install requests

# 运行（默认连 esp-web.local:81/stream）
python D:\WHR\program\iot\camera\imagebase\capture_frames.py

# 指定 IP
python capture_frames.py 192.168.1.100
```

**功能：**
- 从 MJPEG 流抓取 JPEG 帧，10fps 存盘
- 文件名格式：`20260707_173021_3.jpg`（年月日_时分秒_十分之一秒）
- 1 分钟滚动缓冲区（最多 600 张，超出自动删最旧）
- 断连自动重连

## 八、证据文件

| 文件 | 内容 |
|------|------|
| `.omo/evidence/task-1-crop-macro-fix.txt` | 宏名修正 + vTaskDelay 验证 |
| `.omo/evidence/task-2-sdkconfig-verify.txt` | sdkconfig 配置变更验证 |
| `.omo/evidence/task-3-build-output.txt` | 编译输出 (1156 targets, 0 errors) |

## 九、待验证

- [ ] COM7 端口可用时烧录固件
- [ ] 浏览器访问 `http://<P4-IP>` 确认视频流流畅
- [ ] 浏览器 DevTools 中看 X-Timestamp 确认帧率约 15fps
- [ ] 30 秒连续播放无卡顿
