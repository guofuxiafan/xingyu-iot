# network-voice-json — Work Plan

## TL;DR (For humans)

**What you'll get**: ws_server.py 提供两个 HTTP API：`POST /voice`（手动播报）和 `POST /api/v1/fitness/coaching-events`（动作检测系统回调自动播报），均通过现有 WebSocket（端口 8765）将文本发送到 ESP32 离线 TTS 实时播报。与 SPIFFS 启动问候语互补共存。

**Why this approach**: 
- 复用现有全双工 WebSocket 连接，零新端口/新协议
- **WS 回调仅 xQueueSend 投递**（< 1ms），不解码 JSON、不做 TTS、不拿任何锁，因此不阻塞 `client->lock`，JPEG 推流完全不受影响
- voice_task 统一调度两条路径（SPIFFS 轮询 + WS 队列），互斥锁保护 TTS 引擎

**What it will NOT do**: 不删 SPIFFS 轮询、不改 HTTP Server、不影响 JPEG 推流、不改摄像头采集。

**Effort**: 4 waves, 4 todos, ~1.5h。

**Decisions**: WS 双向文本帧 | POST /voice HTTP API | SPIFFS 保留互补 | WS 回调异步 xQueueSend 投递 | voice_task 统一调度 | 双路径互斥锁保护

---

## Scope

### IN
- voice_output.h/c: 公开 `voice_output_speak_json()` + 互斥锁（保护双路径）+ FreeRTOS 队列
- ws_streamer.c: WS 事件回调 → xQueueSend 投递 + 注册 esp_websocket_register_events
- ws_server.py: 保存 WS 连接 + POST /voice HTTP API + send_voice_to_esp32 + 预览页面语音输入框

### OUT
- ESP32 HTTP Server、SPIFFS 轮询、摄像头采集/编码、Vue 前端

---

## Verification strategy
test-after — 串口日志 + Python 手动验证，每个 todo 带明确 QA 步骤。

---

## Execution strategy

| Wave | 内容 | 依赖 |
|------|------|------|
| W1 | voice_output 改造：互斥锁 + 队列 + 公开 API | 无 |
| W2 | ws_streamer 改造：注册事件回调 + xQueueSend 投递 | W1 |
| W3 | ws_server.py HTTP API: /voice + /coaching-events + 双向 WS | W2 |
| W4 | 端到端集成验收 | W3 |

---

## Todos

### W1-T1: voice_output 改造：互斥锁保护双路径 + 队列 + 公开 API

**Refs**: voice_output.c:227(speak_json_value), :202(speak_text), :318(process_json_file), :395(voice_task), voice_output.h

**Impl**:

**Step 1 — 互斥锁**: 在 voice_output.c 中新增 `static SemaphoreHandle_t s_tts_mutex`，在 `voice_output_start()` 中 `s_tts_mutex = xSemaphoreCreateMutex()`。

**Step 2 — 保护现有 SPIFFS 路径**: 在 `process_json_file()` (voice_output.c:318) 中，将调用 `speak_json_value(root)` (原 line 356) 包裹在互斥锁中：
```c
xSemaphoreTake(s_tts_mutex, portMAX_DELAY);
esp_err_t ret = speak_json_value(root);
xSemaphoreGive(s_tts_mutex);
```
这样两条路径（SPIFFS 轮询 + 网络 WS）都受同一把锁保护，不会同时访问 `s_tts`。

**Step 3 — 队列**: 新增 `static QueueHandle_t s_voice_queue`（元素类型 `char *`），长度 8。在 `voice_output_start()` 中 `s_voice_queue = xQueueCreate(8, sizeof(char *))`。

**Step 4 — 公开 API**: voice_output.h 声明：
```c
esp_err_t voice_output_speak_json(const char *json_str);  // 外部调用 → xQueueSend
```

`voice_output_speak_json` 实现（跑在 WS 回调线程，不拿锁，不阻塞）：
```c
esp_err_t voice_output_speak_json(const char *json_str) {
    if (!s_started || !json_str) return ESP_ERR_INVALID_STATE;
    char *copy = strdup(json_str);
    if (!copy) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_voice_queue, &copy, 0) != pdTRUE) {
        free(copy);
        return ESP_ERR_TIMEOUT;  // 队列满，丢弃（日志 warn）
    }
    return ESP_OK;
}
```

**Step 5 — voice_task 消费队列**: 修改 `voice_task()` (voice_output.c:395)，在 SPIFFS 轮询之前先消费队列：
```c
char *json = NULL;
while (xQueueReceive(s_voice_queue, &json, 0) == pdTRUE) {
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root) {
        xSemaphoreTake(s_tts_mutex, portMAX_DELAY);
        speak_json_value(root);
        xSemaphoreGive(s_tts_mutex);
        cJSON_Delete(root);
    }
}
// 然后执行原有的 poll_json_directory()（已在 process_json_file 内拿锁）
```

**Acceptance**: 
- voice_output_speak_json 被外部调用时立即返回（非阻塞），< 1ms
- 两条路径（SPIFFS + WS）同时触发时互斥排队，不损坏 TTS 状态
- 队列满时返回 ESP_ERR_TIMEOUT，不阻塞

**QA Happy**: app_main 中调用 `voice_output_speak_json("{\"text\":\"测试\"}")`，返回 ESP_OK，voice_task 取出后喇叭播放。
**QA Fail**: 连续发送 10 条 JSON（超过队列长度 8），前 8 条返回 ESP_OK，后 2 条返回 ESP_ERR_TIMEOUT，日志 warn。
**Commit**: `feat(voice): add mutex, queue, and public voice_output_speak_json() API`

---

### W2-T2: ws_streamer 改造：注册 WS 事件回调 + xQueueSend 投递

**Refs**: 
- ws_streamer.c:164-185 (ws_streamer_start)
- ws_streamer.c:17 (WS_QUEUE_LEN), :95-127 (ws_send_task 结构)
- esp_websocket_client.c:1366 (client->lock held during recv), :710 (send_bin needs lock)
- managed_components/espressif__esp_websocket_client/examples/target/main/websocket_example.c:193 (register_events 示例)
- sdkconfig:3024 — CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=n（**未启用**，因此必须用队列异步避免卡 JPEG 发送）

**Impl**:

**Step 1 — 注册事件回调**: 在 `ws_streamer_start()` 中 `esp_websocket_client_start(s_ws_client)` 之前添加：
```c
esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_DATA, ws_event_handler, NULL);
#include "voice_output.h"  // 新增头文件
```

**Step 2 — 回调实现**（仅投递，不解析，不拿锁）:
```c
static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    // 只处理完整的文本帧
    if (data->op_code != 0x01) return;
    if (!data->fin) {
        ESP_LOGW(TAG, "voice: fragmented WS message ignored");
        return;
    }
    
    // 复制数据到堆（WS 库可能复用 data_ptr 指向的 rx_buffer）
    char *json = malloc(data->data_len + 1);
    if (!json) {
        ESP_LOGW(TAG, "voice: OOM, dropping WS text frame");
        return;
    }
    memcpy(json, data->data_ptr, data->data_len);
    json[data->data_len] = '\0';
    
    // 异步投递 → voice_task 统一调度，不在此回调中做任何阻塞操作
    esp_err_t ret = voice_output_speak_json(json);
    free(json);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "voice: queue full, dropped: %s", esp_err_to_name(ret));
    }
}
```

**关键设计约束**（由 Oracle 审查确认）:
- WS 事件回调在 `esp_websocket_client_recv` 内同步执行，此时 `client->lock` 已持有
- `esp_websocket_client_send_bin`（ws_send_task 使用的 JPEG 发送函数）也需要 `client->lock`
- 因此回调**绝不能**做耗时操作（TTS 合成 2-5s 会直接卡死 JPEG 推流）
- 本设计的回调仅 malloc + memcpy + xQueueSend(0) → 总耗时 < 1ms，`client->lock` 瞬间释放

**Acceptance**: 
- PC 通过 WS 发文本帧 → ESP32 回调收到 → xQueueSend 投递 → 回调立即返回
- voice_task 取出队列内容 → 互斥锁排队 → TTS 合成播放
- JPEG 推流在语音播放期间帧率无明显下降（±5%）
- 碎片化 WS 消息被拒绝（日志 warn），不崩溃

**QA Happy**: PC 发 WS 文本帧 {"text":"你好"}，串口日志顺序出现 `ws_event_handler → voice_output_speak_json → voice_task → speaking: 你好`，喇叭播放。同时观察 JPEG 帧计数器，帧率不变。
**QA Fail**: PC 发超长 JSON (>16KB) → 碎片化，ESP32 日志 `fragmented WS message ignored`，不崩溃。
**Commit**: `feat(ws): register event handler, xQueueSend voice JSON to voice_task`

---

### W3-T3: ws_server.py HTTP API + 双向 WS

**Refs**: ws_server.py:104-150 (websocket_handler), ws_server.py:218-253 (http_handler)

**Design**: 在 ws_server.py 的现有 HTTP server（端口 8081）上新增 `POST /voice` 端点。请求格式：

```json
POST /voice
Content-Type: application/json

{"text": "要播报的中文内容"}
```

ws_server.py 将文本包装成 ESP32 兼容的 JSON 格式 `{"items":[{"text":"..."}]}`，通过已建立的 WebSocket 连接（端口 8765）以文本帧发送到 ESP32。

**Impl**:

**Step 1 — 保存 WS 连接引用**:
```python
from websockets import WebSocketServerProtocol
import websockets

active_ws_connections: dict[str, WebSocketServerProtocol] = {}

# 在 websocket_handler 开头:
client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
active_ws_connections[client_ip] = websocket
print(f"[WS] Client {client_ip} connected (voice ready)")

# 在 websocket_handler 的 finally 块:
try:
    async for message in websocket:
        # ... 现有 JPEG 帧处理（不变）...
finally:
    active_ws_connections.pop(client_ip, None)
    print(f"[WS] Client {client_ip} disconnected")
```

**Step 2 — 发送函数**:
```python
import json as json_module

async def send_voice_to_esp32(text: str) -> tuple[bool, str]:
    """Send Chinese text to ESP32 for TTS broadcast.
    Returns (success, message)."""
    if not active_ws_connections:
        return False, "No ESP32 connected"
    
    # 包装为 ESP32 voice_output 兼容的 JSON 格式
    payload = json_module.dumps(
        {"items": [{"text": text}]},
        ensure_ascii=False
    )
    
    sent_to = []
    dead = []
    for ip, ws in list(active_ws_connections.items()):
        try:
            await ws.send(payload)  # WebSocket 文本帧 (opcode=0x1)
            sent_to.append(ip)
        except (websockets.exceptions.ConnectionClosed,
                websockets.exceptions.ConnectionClosedOK,
                websockets.exceptions.ConnectionClosedError):
            dead.append(ip)
        except Exception as e:
            print(f"[voice] Send error to {ip}: {e}")
            dead.append(ip)
    
    for ip in dead:
        active_ws_connections.pop(ip, None)
    
    if sent_to:
        return True, f"Sent to {', '.join(sent_to)}"
    elif dead:
        return False, "ESP32 disconnected during send"
    else:
        return False, "Send failed (unknown error)"
```

**Step 3 — HTTP POST /voice 处理** (在 `http_handler` 函数中新增):
```python
# 在 http_handler 中，现有路由逻辑之后添加:

if b"POST /voice" in request:
    # 提取请求体（在 headers 后的 \r\n\r\n 之后）
    header_end = request.find(b"\r\n\r\n")
    if header_end < 0:
        writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        await writer.drain()
        return
    
    body = request[header_end + 4:]
    try:
        data = json_module.loads(body.decode("utf-8"))
        text = data.get("text", "").strip()
    except (json_module.JSONDecodeError, UnicodeDecodeError):
        text = ""
    
    if not text:
        response_body = json_module.dumps({"ok": False, "error": "Missing or invalid 'text' field"})
        writer.write(
            f"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(response_body)}\r\nConnection: close\r\n\r\n"
            f"{response_body}".encode()
        )
        await writer.drain()
        return
    
    success, msg = await send_voice_to_esp32(text)
    response_body = json_module.dumps({"ok": success, "message": msg}, ensure_ascii=False)
    status = "200 OK" if success else "503 Service Unavailable"
    writer.write(
        f"HTTP/1.1 {status}\r\nContent-Type: application/json\r\n"
        f"Content-Length: {len(response_body)}\r\nConnection: close\r\n\r\n"
        f"{response_body}".encode()
    )
    await writer.drain()
    print(f"[voice] POST /voice text=\"{text[:30]}...\" -> {msg}")
    return
```

### W3-T3 补充: POST /api/v1/fitness/coaching-events (动作检测回调)

**Refs**: 外部接口文档 `D:\VV\WeChatFiles\xwechat_files\wxid_eqz00tzziqqc22_ccf6\msg\file\2026-07\动作检测异常回调接口.md`

**设计**: 接收第三方动作检测系统的回调，当检测到运动姿态异常时自动将教练建议语音播报。

**请求格式**:
```json
POST /api/v1/fitness/coaching-events
Content-Type: application/json

{
  "schema_version": "1.0",
  "event_id": "01J4ABCDEF123456",
  "event_type": "pose_coaching_advice",
  "occurred_at": "2026-07-25T13:17:20.899+08:00",
  "session": { "session_id": "...", "exercise": "pushup", ... },
  "coaching": {
    "suggestion": "收紧核心，抬高臀部，保持身体呈直线。",
    "fallback": false,
    "model": "qwen3.7-plus"
  }
}
```

**响应格式**:
```json
{"accepted": true, "event_id": "01J4ABCDEF123456"}
```

**Impl** (在 `http_handler` 中，POST /voice 路由之后添加):

```python
elif b"POST /api/v1/fitness/coaching-events" in request_line:
    header_end = request.find(b"\r\n\r\n")
    if header_end < 0:
        writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        await writer.drain()
        writer.close()
        return
    
    body = request[header_end + 4:]
    
    try:
        event = json_module.loads(body.decode("utf-8"))
        event_id = event.get("event_id", "unknown")
        suggestion = event.get("coaching", {}).get("suggestion", "").strip()
    except (json_module.JSONDecodeError, UnicodeDecodeError, AttributeError):
        suggestion = ""
        event_id = "unknown"
    
    if not suggestion:
        # 无 suggestion 字段，仍接受但不播报
        response = json_module.dumps({"accepted": False, "event_id": event_id,
                                       "error": "missing coaching.suggestion"})
        writer.write(
            f"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(response)}\r\nConnection: close\r\n\r\n"
            f"{response}".encode()
        )
        await writer.drain()
        writer.close()
        return
    
    # 提取 suggestion 文本 → 语音播报
    success, msg = await send_voice_to_esp32(suggestion)
    
    response = json_module.dumps({"accepted": success, "event_id": event_id},
                                  ensure_ascii=False)
    writer.write(
        f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        f"Content-Length: {len(response)}\r\nConnection: close\r\n\r\n"
        f"{response}".encode()
    )
    await writer.drain()
    print(f"[coaching] event={event_id} suggestion=\"{suggestion[:40]}...\" -> {msg}")
    writer.close()
    return
```

**完整路由顺序** (http_handler 内的判断优先级):
```
1. GET /stream_m|/stream_l|/stream_r  → MJPEG stream (不变)
2. GET /                                → 预览页面 (不变)
3. POST /voice                          → 语音播报 (W3 核心)
4. POST /api/v1/fitness/coaching-events → 动作检测回调 (新增)
5. 其他                                 → 404
```

**Acceptance**:
- `curl -X POST http://localhost:8081/api/v1/fitness/coaching-events -H "Content-Type: application/json" -d '{"event_id":"test-001","coaching":{"suggestion":"收紧核心"}}'` → 返回 `{"accepted":true,"event_id":"test-001"}` → ESP32 播报 "收紧核心"
- 缺少 `coaching.suggestion` → 返回 400 `{"accepted":false,"error":"missing coaching.suggestion"}`
- 无 ESP32 连接 → 返回 `{"accepted":false,"event_id":"..."}` (失败但不影响调用方)

**QA Happy**: 完整回调 JSON → 返回 200 → ESP32 播报 suggestion 内容。
**QA Fail**: 空 body / 非法 JSON → 返回 400，不崩溃。ESP32 断连 → 返回 accepted:false。
**Commit**: 合并到 `feat(ws_server): add POST /voice and coaching callback HTTP APIs`

**完整 http_handler 路由逻辑**（伪代码，实现时按现有结构插入）:
```
if GET /stream_m|/stream_l|/stream_r  → MJPEG stream (不变)
elif GET /                                → 预览页面 (不变)
elif POST /voice                          → 语音播报 (W3 核心)
elif POST /api/v1/fitness/coaching-events → 动作检测回调 (新增)
else                                      → 404
```

**Step 4 — 更新预览页面** (render_index 函数): 在现有 HTML 中添加语音输入区域（可选，轻量 UI）:
```html
<div style="margin-top: 20px; padding: 15px; background: #2a2a2a; border-radius: 10px;">
  <h2 style="font-size: 16px; margin: 0 0 10px 0;">Voice Broadcast</h2>
  <input id="voiceText" type="text" placeholder="输入中文播报内容..." 
         style="width: 70%; padding: 8px; border-radius: 5px; border: 1px solid #555; background: #1a1a1a; color: #fff;">
  <button onclick="sendVoice()" style="padding: 8px 20px; margin-left: 10px; border-radius: 5px; border: none; background: #4CAF50; color: white; cursor: pointer;">发送播报</button>
  <span id="voiceStatus" style="margin-left: 10px; font-size: 14px; color: #888;"></span>
</div>
<script>
async function sendVoice() {
  const text = document.getElementById('voiceText').value.trim();
  const status = document.getElementById('voiceStatus');
  if (!text) { status.textContent = '请输入内容'; return; }
  status.textContent = '发送中...';
  try {
    const res = await fetch('/voice', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({text}) });
    const data = await res.json();
    status.textContent = data.ok ? '\u2705 ' + data.message : '\u274c ' + data.message;
    if (data.ok) document.getElementById('voiceText').value = '';
  } catch(e) { status.textContent = '\u274c 请求失败'; }
}
</script>
```

**API 文档**:

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/voice` | 发送文本到 ESP32 语音播报 |
| POST | `/api/v1/fitness/coaching-events` | 动作检测回调，自动播报教练建议 |

### POST /voice

请求体:
```json
{"text": "要播报的中文内容"}
```

成功响应 (200):
```json
{"ok": true, "message": "Sent to 192.168.1.100"}
```

失败响应:
- `400` — `{"ok": false, "error": "Missing or invalid 'text' field"}`（text 字段缺失或为空）
- `503` — `{"ok": false, "message": "No ESP32 connected"}`（无设备连接）

**Acceptance**:
- `curl -X POST http://localhost:8081/voice -H "Content-Type: application/json" -d '{"text":"你好"}'` → 返回 200，ESP32 喇叭播放
- 无 ESP32 连接时返回 503
- text 字段为空时返回 400
- ESP32 断连时自动清理，下次请求返回 503

**QA Happy**: curl POST /voice → 返回 `{"ok":true,...}` → ESP32 串口 `voice_task: speaking: 你好` → 喇叭播放。
**QA Fail**: 无 ESP32 连接 → curl → 返回 503 `{"ok":false,"message":"No ESP32 connected"}`。空 text → 返回 400。
**Commit**: `feat(ws_server): add POST /voice and coaching callback HTTP APIs`

---

### W4-T4: 端到端集成验收

**Refs**: 以上所有

**Verification**:
1. 烧录固件 → 启动 ws_server.py
2. ESP32 连上 → JPEG 推流正常
3. `curl -X POST http://localhost:8081/voice -H "Content-Type: application/json" -d '{"text":"网络语音测试"}'`
4. → 返回 200 OK → ESP32 喇叭播放 "网络语音测试"
5. 语音播放期间 JPEG 帧率无明显下降（±5%）
6. 浏览器打开 `http://localhost:8081/` → 预览页面显示语音输入框 → 输入中文 → 点发送 → 喇叭播放
7. SPIFFS /storage/voice.json 启动问候仍正常
8. 断开 ESP32 → curl POST /voice → 返回 503 → ws_server.py 不崩溃 → 重新连接后恢复正常

**Acceptance**: HTTP API 延迟 < 1s，端到端延迟（请求 → 喇叭）< 5s，JPEG 帧率波动 < 5%。

**QA**: 逐项验证以上 8 步。
**Commit**: `test: end-to-end HTTP voice API delivery`

---

## Final verification wave

| 检查 | 方法 | 预期 |
|------|------|------|
| F1 代码审查 | 读 diff，确认 WS 回调无耗时操作 | WS 回调仅 malloc+memcpy+xQueueSend(0)，无 cJSON_Parse、无 TTS、无 I2S |
| F2 互斥正确性 | 验证 s_tts_mutex 覆盖 process_json_file 和 voice_task 消费路径 | 两路径均拿锁 |
| F3 功能回归 | JPEG 帧计数对比（语音播放前/中/后） | ±5% |
| F4 边界 | 空 JSON / 超长 / 队列满 / WS 断连 | 不崩溃，日志 warn |
| F5 内存安全 | 检查所有 malloc/free + xQueueSend 配对 | 无泄漏 |

---

## Commit strategy

4 commits: `feat(voice): mutex+queue+public API` → `feat(ws): xQueueSend voice via event handler` → `feat(ws_server): add POST /voice and coaching callback HTTP APIs` → `test: end-to-end HTTP voice API delivery`

## Success criteria

- [ ] POST /voice → ESP32 自动播报（端到端 < 5s）
- [ ] 语音播放期间 JPEG 推流帧率波动 < 5%
- [ ] SPIFFS 启动问候语正常
- [ ] 异常场景（队列满 / WS 断连 / HTTP 400/503）不崩溃
