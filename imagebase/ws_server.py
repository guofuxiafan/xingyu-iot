#!/usr/bin/env python3
"""
WebSocket server to receive JPEG frames from ESP32-P4 and serve local MJPEG stream.
"""

import asyncio
import struct
import os
from datetime import datetime
from pathlib import Path

import websockets

# Configuration
WS_PORT = 8765
HTTP_PORT = 8080
FRAMES_DIR = Path(r"D:\WHR\program\iot\camera\imagebase\ws_frames")
FRAMES_DIR.mkdir(parents=True, exist_ok=True)

# Global state
latest_frame: bytes | None = None
latest_frame_info: dict | None = None
frame_counter = 0
frame_queue: asyncio.Queue = asyncio.Queue(maxsize=2)


def parse_frame(data: bytes) -> tuple[int, int, int, bytes]:
    """
    Parse frame from ESP32-P4.
    Format: [timestamp_ms:8 LE uint64][camera_id:1 uint8][data_len:2 BE uint16][jpeg:data_len]
    Returns: (timestamp_ms, camera_id, jpeg_data)
    """
    timestamp_ms = struct.unpack("<Q", data[0:8])[0]
    camera_id = data[8]
    data_len = struct.unpack(">H", data[9:11])[0]
    jpeg_data = data[11:11 + data_len]
    return timestamp_ms, camera_id, jpeg_data


def save_frame(timestamp_ms: int, camera_id: int, jpeg_data: bytes) -> Path:
    """Save JPEG frame to disk with timestamped filename (uses PC local time + P4 ms for decisecond)."""
    now = datetime.now()
    decisecond = now.microsecond // 100000  # 0-9
    filename = f"cam{camera_id}_{now.strftime('%Y%m%d_%H%M%S')}_{decisecond}.jpg"
    filepath = FRAMES_DIR / filename
    with open(filepath, "wb") as f:
        f.write(jpeg_data)
    return filepath


MAX_FRAMES = 600


def cleanup_old_frames():
    """Delete oldest frames if total exceeds MAX_FRAMES."""
    files = sorted(FRAMES_DIR.glob("*.jpg"), key=lambda f: f.stat().st_mtime)
    while len(files) > MAX_FRAMES:
        oldest = files.pop(0)
        oldest.unlink()


async def websocket_handler(websocket):
    """Handle incoming WebSocket connections and receive frames."""
    global latest_frame, latest_frame_info, frame_counter

    client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    print(f"[WS] Client connected from {client_ip}")

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                try:
                    timestamp_ms, camera_id, jpeg_data = parse_frame(message)

                    frame_counter += 1

                    # Save to disk
                    filepath = save_frame(timestamp_ms, camera_id, jpeg_data)

                    # Cleanup oldest every 50 frames
                    if frame_counter % 50 == 0:
                        cleanup_old_frames()

                    # Update latest frame
                    latest_frame = jpeg_data
                    latest_frame_info = {
                        "camera_id": camera_id,
                        "timestamp_ms": timestamp_ms,
                        "size": len(jpeg_data),
                        "filepath": str(filepath),
                    }

                    # Print status every 50 frames
                    if frame_counter % 50 == 0:
                        print(
                            f"已接收 {frame_counter} 帧, "
                            f"cam{camera_id}, "
                            f"最新帧 {len(jpeg_data)} bytes, "
                            f"时间戳 {timestamp_ms}ms"
                        )

                    # Put frame in queue for MJPEG subscribers
                    try:
                        if frame_queue.full():
                            frame_queue.get_nowait()
                        frame_queue.put_nowait(jpeg_data)
                    except asyncio.QueueFull:
                        pass

                except struct.error as e:
                    print(f"[WS] Parse error: {e}")
                except Exception as e:
                    print(f"[WS] Frame error: {e}")
    except websockets.exceptions.ConnectionClosed:
        print(f"[WS] Client {client_ip} disconnected")
    except Exception as e:
        print(f"[WS] Error: {e}")

    print(f"[WS] Waiting for new connection...")


async def http_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Handle HTTP requests for MJPEG stream and index page."""
    global latest_frame

    try:
        request = await reader.read(4096)
        if not request:
            return

        if b"GET /stream" in request:
            # MJPEG multipart stream
            boundary = "frame"
            writer.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n"
                b"Cache-Control: no-cache, no-store, must-revalidate\r\n"
                b"Connection: close\r\n"
                b"\r\n"
            )
            await writer.drain()

            while True:
                try:
                    frame = await asyncio.wait_for(frame_queue.get(), timeout=30)
                except asyncio.TimeoutError:
                    # Send latest frame if available, otherwise wait
                    if latest_frame:
                        frame = latest_frame
                    else:
                        continue

                # MJPEG frame
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

        elif b"GET / " in request or b"GET /" in request:
            # HTML index page
            html = (
                "<!DOCTYPE html>\n"
                "<html>\n"
                "<head>\n"
                "<title>ESP32-P4 Camera Stream</title>\n"
                "<style>\n"
                "body { font-family: Arial; text-align: center; margin: 0; padding: 20px; background: #1a1a1a; color: #fff; }\n"
                "h1 { margin-bottom: 20px; }\n"
                "img { max-width: 90%; border: 2px solid #333; border-radius: 8px; }\n"
                ".status { margin-top: 15px; padding: 10px; background: #333; border-radius: 5px; display: inline-block; }\n"
                "</style>\n"
                "</head>\n"
                "<body>\n"
                "<h1>ESP32-P4 Camera Stream</h1>\n"
                '<img id="stream" src="/stream" alt="Waiting for camera...">\n'
                "<div class='status' id='status'>Waiting for camera...</div>\n"
                "<script>\n"
                "let img = document.getElementById('stream');\n"
                "let status = document.getElementById('status');\n"
                "img.onload = () => { status.textContent = 'Streaming...'; };\n"
                "img.onerror = () => { status.textContent = 'Waiting for camera...'; };\n"
                "setTimeout(() => { if (!img.src.includes('stream')) location.reload(); }, 5000);\n"
                "</script>\n"
                "</body>\n"
                "</html>\n"
            )

            body = html.encode()
            writer.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/html\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"Connection: close\r\n"
                b"\r\n"
            )
            writer.write(body)

        else:
            # 404
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
    # Start HTTP server for MJPEG
    http_server = await asyncio.start_server(http_handler, "0.0.0.0", HTTP_PORT)
    print(f"[HTTP] MJPEG stream server running on http://localhost:{HTTP_PORT}")
    print(f"[HTTP] Index page: http://localhost:{HTTP_PORT}/")
    print(f"[HTTP] Stream endpoint: http://localhost:{HTTP_PORT}/stream")

    # Start WebSocket server
    ws_server = await websockets.serve(websocket_handler, "0.0.0.0", WS_PORT)
    print(f"[WS] WebSocket server running on ws://0.0.0.0:{WS_PORT}")
    print(f"[WS] Frames will be saved to: {FRAMES_DIR}")
    print(f"[WS] Waiting for ESP32-P4 connection...")

    # Run both servers
    await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[Server] Shutting down...")
