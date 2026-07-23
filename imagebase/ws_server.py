#!/usr/bin/env python3
"""
WebSocket server to receive JPEG frames from ESP32-P4 and serve local MJPEG streams.
"""

import asyncio
import struct
from datetime import datetime
from pathlib import Path

import websockets

WS_PORT = 8765
HTTP_PORT = 8081
MAX_FRAMES = 600

# Fixed routing:
# cam0 = CSI, cam1 = USB (direct Type-A1 or HUB), cam2 = second HUB USB.
FRAMES_DIRS = {
    0: Path(r"D:\WHR\program\iot\camera\imagebase\ws_frames_m"),
    1: Path(r"D:\WHR\program\iot\camera\imagebase\ws_frames_l"),
    2: Path(r"D:\WHR\program\iot\camera\imagebase\ws_frames_r"),
}

STREAM_ROUTES = {
    "/stream_m": 0,
    "/stream_l": 1,
    "/stream_r": 2,
}

for frames_dir in FRAMES_DIRS.values():
    frames_dir.mkdir(parents=True, exist_ok=True)

latest_frames: dict[int, bytes] = {}
latest_frame_infos: dict[int, dict] = {}
frame_counters: dict[int, int] = {0: 0, 1: 0, 2: 0}
frame_queues: dict[int, asyncio.Queue] = {i: asyncio.Queue(maxsize=2) for i in range(3)}


def parse_frame(data: bytes) -> tuple[int, int, bytes]:
    """Parse [timestamp_ms:8 LE][camera_id:1][jpeg_len:4 BE][jpeg]."""
    if len(data) < 13:
        raise ValueError(f"Frame too short: {len(data)} bytes")

    timestamp_ms = struct.unpack("<Q", data[0:8])[0]
    camera_id = data[8]
    data_len = struct.unpack(">I", data[9:13])[0]
    if len(data) < 13 + data_len:
        raise ValueError(f"Frame truncated: expected {data_len} bytes, got {len(data) - 13}")

    jpeg_data = data[13:13 + data_len]
    if len(jpeg_data) < 16:
        raise ValueError(f"JPEG payload too short: cam{camera_id}, size={len(jpeg_data)}")

    # Some DMA/encoder paths can leave padding around an otherwise complete
    # JPEG. Normalize to the first SOI...EOI image so only decodable bytes are
    # published and written to disk.
    if jpeg_data[:2] != b"\xff\xd8" or jpeg_data[-2:] != b"\xff\xd9":
        soi = jpeg_data.find(b"\xff\xd8")
        eoi = jpeg_data.find(b"\xff\xd9", soi + 2) if soi >= 0 else -1
        if soi < 0 or eoi < 0:
            head = jpeg_data[:4].hex(" ")
            tail = jpeg_data[-4:].hex(" ")
            raise ValueError(
                f"Invalid JPEG payload: cam{camera_id}, size={len(jpeg_data)}, "
                f"head={head}, tail={tail}"
            )

        normalized_len = eoi + 2 - soi
        print(
            f"[WS] cam{camera_id} normalized JPEG: "
            f"payload={len(jpeg_data)}, jpeg={normalized_len}, prefix={soi}, "
            f"suffix={len(jpeg_data) - (eoi + 2)}"
        )
        jpeg_data = jpeg_data[soi:eoi + 2]

    return timestamp_ms, camera_id, jpeg_data


def save_frame(timestamp_ms: int, camera_id: int, jpeg_data: bytes) -> Path:
    """Save JPEG frame to the fixed per-camera directory."""
    if camera_id not in FRAMES_DIRS:
        raise ValueError(f"Unsupported camera_id: {camera_id}")

    now = datetime.now()
    decisecond = now.microsecond // 100000
    filename = f"cam{camera_id}_{now.strftime('%Y%m%d_%H%M%S')}_{decisecond}_{timestamp_ms}.jpg"
    filepath = FRAMES_DIRS[camera_id] / filename
    with open(filepath, "wb") as f:
        f.write(jpeg_data)
    return filepath


def cleanup_old_frames(camera_id: int) -> None:
    """Delete oldest frames if total exceeds MAX_FRAMES for this camera directory."""
    frames_dir = FRAMES_DIRS[camera_id]
    files = sorted(frames_dir.glob("*.jpg"), key=lambda f: f.stat().st_mtime)
    while len(files) > MAX_FRAMES:
        oldest = files.pop(0)
        oldest.unlink()
        print(f"[cleanup] cam{camera_id} deleted old frame: {oldest.name}")


async def websocket_handler(websocket):
    """Receive ESP32-P4 frames and route them by camera id."""
    client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    print(f"[WS] Client connected from {client_ip}")

    try:
        async for message in websocket:
            if not isinstance(message, bytes):
                continue

            try:
                timestamp_ms, camera_id, jpeg_data = parse_frame(message)
                filepath = save_frame(timestamp_ms, camera_id, jpeg_data)

                frame_counters[camera_id] = frame_counters.get(camera_id, 0) + 1
                if frame_counters[camera_id] % 50 == 0:
                    cleanup_old_frames(camera_id)
                    print(
                        f"[WS] cam{camera_id} received {frame_counters[camera_id]} frames, "
                        f"latest {len(jpeg_data)} bytes -> {filepath}"
                    )

                latest_frames[camera_id] = jpeg_data
                latest_frame_infos[camera_id] = {
                    "camera_id": camera_id,
                    "timestamp_ms": timestamp_ms,
                    "size": len(jpeg_data),
                    "filepath": str(filepath),
                }

                q = frame_queues.get(camera_id)
                if q:
                    if q.full():
                        q.get_nowait()
                    q.put_nowait(jpeg_data)

            except struct.error as e:
                print(f"[WS] Parse error: {e}")
            except Exception as e:
                print(f"[WS] Frame error: {e}")

    except websockets.exceptions.ConnectionClosed:
        print(f"[WS] Client {client_ip} disconnected")
    except Exception as e:
        print(f"[WS] Error: {e}")

    print("[WS] Waiting for new connection...")


async def send_mjpeg_stream(writer: asyncio.StreamWriter, camera_id: int) -> None:
    boundary = "frame"
    writer.write(
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n"
        b"Cache-Control: no-cache, no-store, must-revalidate\r\n"
        b"Connection: close\r\n"
        b"\r\n"
    )
    await writer.drain()

    q = frame_queues.get(camera_id)
    while True:
        try:
            if q:
                frame = await asyncio.wait_for(q.get(), timeout=30)
            else:
                await asyncio.sleep(1)
                continue
        except asyncio.TimeoutError:
            frame = latest_frames.get(camera_id)
            if not frame:
                continue

        frame_bytes = (
            f"--{boundary}\r\n"
            f"Content-Type: image/jpeg\r\n"
            f"Content-Length: {len(frame)}\r\n\r\n"
        ).encode() + frame + b"\r\n"

        try:
            writer.write(frame_bytes)
            await writer.drain()
        except (ConnectionResetError, BrokenPipeError):
            break


def render_index() -> bytes:
    html = (
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>ESP32-P4 Camera Preview</title>\n"
        "<style>\n"
        "body { font-family: Arial; text-align: center; margin: 0; padding: 20px; background: #1a1a1a; color: #fff; }\n"
        "h1 { margin-bottom: 20px; }\n"
        ".cameras { display: flex; flex-wrap: wrap; justify-content: center; gap: 20px; }\n"
        ".camera-card { background: #2a2a2a; border-radius: 10px; padding: 15px; }\n"
        ".camera-card h2 { font-size: 16px; margin: 0 0 10px 0; }\n"
        "img { max-width: 400px; border: 2px solid #444; border-radius: 8px; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>ESP32-P4 Camera Preview</h1>\n"
        '<div class="cameras">\n'
        '  <div class="camera-card"><h2>CSI (cam0)</h2><img src="/stream_m" alt="cam0"></div>\n'
        '  <div class="camera-card"><h2>USB (cam1)</h2><img src="/stream_l" alt="cam1"></div>\n'
        '  <div class="camera-card"><h2>HOST3 (cam2)</h2><img src="/stream_r" alt="cam2"></div>\n'
        "</div>\n"
        "</body>\n"
        "</html>\n"
    )
    return html.encode()


async def http_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Handle HTTP requests for MJPEG streams and index page."""
    try:
        request = await reader.read(4096)
        if not request:
            return

        request_line = request.decode(errors="replace").split("\r\n")[0]
        matched_cam = None
        for route, cam_id in STREAM_ROUTES.items():
            if f"GET {route}" in request_line:
                matched_cam = cam_id
                break

        if matched_cam is not None:
            await send_mjpeg_stream(writer, matched_cam)
        elif b"GET / " in request or b"GET /" in request:
            body = render_index()
            writer.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/html\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"Connection: close\r\n"
                b"\r\n"
            )
            writer.write(body)
            await writer.drain()
        else:
            writer.write(
                b"HTTP/1.1 404 Not Found\r\n"
                b"Content-Length: 0\r\n"
                b"Connection: close\r\n"
                b"\r\n"
            )
            await writer.drain()

    except Exception as e:
        print(f"[HTTP] Error: {e}")
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def main():
    """Start both WebSocket and HTTP servers."""
    await asyncio.start_server(http_handler, "0.0.0.0", HTTP_PORT)
    print(f"[HTTP] Preview page: http://localhost:{HTTP_PORT}/")
    print(f"[HTTP] CSI stream:   http://localhost:{HTTP_PORT}/stream_m")
    print(f"[HTTP] HOST2 stream: http://localhost:{HTTP_PORT}/stream_l")
    print(f"[HTTP] HOST3 stream: http://localhost:{HTTP_PORT}/stream_r")

    await websockets.serve(websocket_handler, "0.0.0.0", WS_PORT)
    print(f"[WS] WebSocket server running on ws://0.0.0.0:{WS_PORT}")
    print(f"[WS] cam0(CSI)   -> {FRAMES_DIRS[0]}")
    print(f"[WS] cam1(USB) -> {FRAMES_DIRS[1]}")
    print(f"[WS] cam2(HOST3) -> {FRAMES_DIRS[2]}")
    print("[WS] Waiting for ESP32-P4 connection...")

    await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[Server] Shutting down...")
