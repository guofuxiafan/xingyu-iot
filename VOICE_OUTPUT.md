# 本地 JSON 离线语音播报

本功能用于 Waveshare ESP32-P4-WIFI6-DEV-KIT 板载 SPK 接口。它会轮询开发板上的
`/storage` 文件夹，发现新增或内容发生变化的 `.json` 文件后，通过乐鑫 ESP-TTS
在设备端离线合成中文语音，并从 8Ω/2W 喇叭播放。
语音任务独立运行；语音初始化失败时只记录警告，不会阻止摄像头服务启动。

电脑端参与 SPIFFS 镜像打包的源文件夹是 `data/`，完整路径为：

```text
D:\WHR\program\iot\camera-feature-b\data
```

烧录到开发板后，该目录挂载为：

```text
/storage
```

## 硬件连接

- 喇叭：接开发板 `SPK` 的 MX1.25 2P 接口，规格为 8Ω/2W。
- Codec：ES8311。
- 功放：NS4150B，GPIO53 控制使能。
- I2S：DOUT=GPIO9、LRCK=GPIO10、DIN=GPIO11、BCLK=GPIO12、MCLK=GPIO13。
- I2C：SDA=GPIO7、SCL=GPIO8，与摄像头 SCCB 共用同一总线。

## JSON 格式

推荐使用 `items` 数组，每一项按顺序播报：

```json
{
  "items": [
    { "text": "摄像头系统启动成功。" },
    { "text": "本地语音播报功能已经就绪。" }
  ]
}
```

同时支持以下形式：

```json
"直接播报这一句话。"
```

```json
{ "text": "直接播报这一句话。" }
```

```json
["第一句话。", "第二句话。"]
```

每个文件必须以 `.json` 结尾并使用 UTF-8 编码。当前 ESP-TTS 仅支持中文，单个文件
最大为 16KB，最多跟踪 32 个文件。默认每 2000ms 扫描一次：

- 新文件：自动读取并播放。
- 已有文件内容改变：重新读取并播放。
- 文件没有变化：不会重复播放。
- 文件删除后以相同名称重新创建：会再次播放。

## 更新与烧录

在电脑端向 `data/` 增加 JSON 后，需要烧录 `storage` 分区。首次使用或改变分区表后，
必须执行完整烧录，以同时写入固件、SPIFFS JSON 和约 2.8MB 的离线音色数据：

```powershell
idf.py flash
```

只执行 `app-flash` 不会更新 JSON 或 `voice_data` 分区。

音量和语速可在 `menuconfig -> Example Configuration` 中调整：

- `Speaker volume (%)`：默认 65。
- `TTS speed`：0 最慢，5 最快，默认 3。
- `JSON folder polling interval (ms)`：目录轮询周期，默认 2000ms。
- 关闭 `Read local JSON aloud through the SPK connector` 可完全禁用功能。
