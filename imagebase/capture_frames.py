#!/usr/bin/env python3
"""
ESP32-P4 视频流抓帧工具

从 ESP32 MJPEG 流中抓取 JPEG 帧，以 10fps 保存到本地，
带 1 分钟滚动缓冲区（最多 600 张，超出自动删最旧）。

用法:
    python capture_frames.py                          # 默认 http://esp-web.local:81/stream
    python capture_frames.py http://192.168.1.100:81/stream
    python capture_frames.py 192.168.1.100            # 自动补全为 http://...:81/stream
"""

import sys
import os
import time
import re
import threading
from datetime import datetime
from pathlib import Path

# ── 配置 ──────────────────────────────────────────────
SAVE_DIR = Path(r"D:\WHR\program\iot\camera\imagebase")
MAX_FILES = 600          # 最多保留 600 张（1 分钟 × 10fps）
TARGET_FPS = 10          # 目标帧率
FRAME_INTERVAL = 1.0 / TARGET_FPS  # 每帧间隔（秒）

DEFAULT_URL = "http://esp-web.local:81/stream"
# ──────────────────────────────────────────────────────


def parse_mjpeg_stream(response, boundary: bytes):
    """从 MJPEG multipart 响应中逐帧提取 JPEG 数据（生成器）"""
    # 确保 boundary 正确处理
    if not boundary.startswith(b"--"):
        boundary = b"--" + boundary

    data = b""
    for chunk in response.iter_content(chunk_size=8192):
        data += chunk
        while boundary in data:
            # 找到 boundary 位置
            idx = data.find(boundary)
            if idx < 0:
                break

            # 检查是否是结束标记
            end_idx = data.find(boundary + b"--", idx)
            if end_idx == idx:
                return  # 流结束

            # 找下一个 boundary
            next_idx = data.find(boundary, idx + len(boundary))
            if next_idx < 0:
                break  # 还没收完这一帧

            part = data[idx:next_idx]

            # 找 JPEG 数据：头部和内容之间由 \r\n\r\n 分隔
            header_end = part.find(b"\r\n\r\n")
            if header_end > 0:
                jpeg_data = part[header_end + 4:]
                # 去掉尾部可能多余的 \r\n
                jpeg_data = jpeg_data.rstrip(b"\r\n")
                if jpeg_data.startswith(b"\xff\xd8") and jpeg_data.endswith(b"\xff\xd9"):
                    yield jpeg_data

            data = data[next_idx:]


def maintain_buffer():
    """清理超出 MAX_FILES 的旧文件（按修改时间排序，删最早的）"""
    files = sorted(SAVE_DIR.glob("*.jpg"), key=lambda f: f.stat().st_mtime)
    while len(files) > MAX_FILES:
        oldest = files.pop(0)
        oldest.unlink()
        print(f"[清理] 删除旧文件: {oldest.name}")


def get_stream_url(arg=None) -> str:
    """解析命令行参数得到流 URL"""
    if arg is None:
        return DEFAULT_URL

    arg = arg.strip()
    # 已经是完整 URL
    if arg.startswith("http://") or arg.startswith("https://"):
        return arg

    # 纯 IP 地址 → 补全
    if re.match(r"^\d+\.\d+\.\d+\.\d+", arg):
        return f"http://{arg}:81/stream"

    # 主机名 → 补全
    return f"http://{arg}:81/stream"


def main():
    # 解析地址
    url = get_stream_url(sys.argv[1] if len(sys.argv) > 1 else None)
    print(f"流地址: {url}")

    # 创建保存目录
    SAVE_DIR.mkdir(parents=True, exist_ok=True)

    # 清理计数器
    cleanup_counter = 0

    print(f"开始抓帧... (目标 {TARGET_FPS}fps, 缓冲区 {MAX_FILES} 张)")
    print(f"保存路径: {SAVE_DIR}")
    print("按 Ctrl+C 停止\n")

    try:
        import requests
    except ImportError:
        print("错误: 需要 requests 库，请运行: pip install requests")
        sys.exit(1)

    session = requests.Session()
    last_frame_time = 0

    while True:
        try:
            response = session.get(url, stream=True, timeout=10)
            content_type = response.headers.get("Content-Type", "")

            if "multipart" not in content_type:
                print(f"错误: 不是 MJPEG 流 (Content-Type: {content_type})")
                time.sleep(3)
                continue

            # 提取 boundary
            boundary_match = re.search(r"boundary=(\S+)", content_type)
            if not boundary_match:
                print("错误: 找不到 multipart boundary")
                time.sleep(3)
                continue

            boundary = boundary_match.group(1).encode()
            if boundary.startswith(b'"') and boundary.endswith(b'"'):
                boundary = boundary[1:-1]

            print(f"已连接，boundary: {boundary.decode(errors='replace')}")

            for jpeg_data in parse_mjpeg_stream(response, boundary):
                now = time.time()

                # 控制帧率：距上一帧不够 FRAME_INTERVAL 就跳过
                if now - last_frame_time < FRAME_INTERVAL:
                    continue
                last_frame_time = now

                # 生成时间戳文件名（精确到 0.1 秒）
                ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                # 加十分之一秒
                decisecond = int((now % 1) * 10)
                filename = f"{ts}_{decisecond}.jpg"
                filepath = SAVE_DIR / filename

                # 写入文件
                with open(filepath, "wb") as f:
                    f.write(jpeg_data)

                print(f"[保存] {filename}  ({len(jpeg_data)} bytes)")

                # 每 50 帧做一次缓冲区清理
                cleanup_counter += 1
                if cleanup_counter >= 50:
                    maintain_buffer()
                    cleanup_counter = 0

        except requests.exceptions.ConnectionError:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] 连接失败，3 秒后重试...")
            time.sleep(3)
        except requests.exceptions.ReadTimeout:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] 读取超时，重连中...")
            time.sleep(1)
        except KeyboardInterrupt:
            print("\n用户中断，退出。")
            break
        except Exception as e:
            print(f"异常: {e}，3 秒后重试...")
            time.sleep(3)


if __name__ == "__main__":
    main()
