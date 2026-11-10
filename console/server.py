"""Two-camera console, JPEG proxy and motion webhook. Python standard library only."""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hmac
import json
from pathlib import Path
import re
import secrets
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from urllib.request import build_opener, HTTPRedirectHandler, ProxyHandler, Request

ROOT = Path(__file__).resolve().parents[1]
MAX_JPEG = 256 * 1024
EVENT_ID = re.compile(r"^[a-f0-9]{32}$")


class LimitedHTTPServer(ThreadingHTTPServer):
    """Fixed admission limit; overload never creates an extra request thread."""
    daemon_threads = True
    request_queue_size = 4

    def __init__(self, address, handler, max_clients=4):
        self.slots = threading.BoundedSemaphore(max_clients)
        super().__init__(address, handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            request.settimeout(5)
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # Never forward device credentials to another host.


def device_request(camera, path):
    url = camera.get("url", "").rstrip("/")
    parsed = urlsplit(url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/"):
        raise ValueError("设备地址必须是 http(s)://主机:端口")
    credentials = (camera.get("username", "cam") + ":" + camera.get("password", "")).encode("utf-8")
    request = Request(url + path, headers={"Authorization": "Basic " + base64.b64encode(credentials).decode("ascii")})
    opener = build_opener(ProxyHandler({}), NoRedirect())
    try:
        with opener.open(request, timeout=2) as response:
            data = response.read(MAX_JPEG + 1)
            if len(data) > MAX_JPEG:
                raise ValueError("设备返回数据过大")
            return response.status, dict(response.headers), data
    except HTTPError as error:
        raise ValueError("设备认证失败" if error.code == 401 else "设备返回 HTTP " + str(error.code)) from error


class EventStore:
    def __init__(self, directory, limit=20):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.limit = max(1, min(int(limit), 500))
        self.lock = threading.Lock()
        self.events = []
        for path in self.directory.glob("*.json"):
            if not EVENT_ID.fullmatch(path.stem):
                continue
            try:
                item = json.loads(path.read_text(encoding="utf-8"))
                if item.get("id") != path.stem or not (self.directory / (path.stem + ".jpg")).is_file():
                    continue
                datetime.fromisoformat(item["received_at"])
                item["score"] = int(item["score"])
                self.events.append({k: item[k] for k in ("id", "received_at", "score", "device_event", "uptime_ms")})
                # Bound startup metadata even if an old directory has many files.
                if len(self.events) > self.limit:
                    self.events.sort(key=lambda event: event["received_at"], reverse=True)
                    self._prune()
            except (OSError, ValueError, KeyError, TypeError):
                continue
        self.events.sort(key=lambda event: event["received_at"], reverse=True)
        self._prune()

    def _prune(self):
        for item in self.events[self.limit:]:
            for suffix in (".jpg", ".json"):
                (self.directory / (item["id"] + suffix)).unlink(missing_ok=True)
        self.events = self.events[:self.limit]

    def add(self, jpeg, score, device_event, uptime):
        if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            raise ValueError("请求体不是完整 JPEG")
        event = {"id": secrets.token_hex(16), "received_at": datetime.now(timezone.utc).isoformat(),
                 "score": score, "device_event": device_event, "uptime_ms": uptime}
        with self.lock:
            path = self.directory / event["id"]
            path.with_suffix(".jpg").write_bytes(jpeg)
            path.with_suffix(".json").write_text(json.dumps(event, ensure_ascii=False), encoding="utf-8")
            self.events.insert(0, event)
            self._prune()
        return event

    def snapshot(self):
        with self.lock:
            return [dict(event) for event in self.events]

    def photo(self, identity):
        if not EVENT_ID.fullmatch(identity):
            return None
        with self.lock:
            if not any(event["id"] == identity for event in self.events):
                return None
            try:
                return (self.directory / (identity + ".jpg")).read_bytes()
            except FileNotFoundError:
                return None


class ConsoleState:
    def __init__(self, config, data_dir):
        self.config = config
        self.store = EventStore(data_dir, config.get("retain_events", 20))
        self.preview_slot = threading.BoundedSemaphore(1)
        self.upload_slot = threading.BoundedSemaphore(1)
        self.preview_fps = max(1, min(int(config.get("preview_fps", 5)), 15))
        self.poll_interval = max(5, min(float(config.get("status_poll_seconds", 10)), 60))
        self.last_viewed = 0.0
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.cameras = {}
        self.status = []
        for identity, role, name in (("stream", "stream", "实时摄像头"), ("detector", "detector", "检测摄像头")):
            camera = dict(config.get(identity, {}))
            camera.update(id=identity, role=role, name=camera.get("name", name))
            self.cameras[identity] = camera
            self.status.append(self.public_status(camera))

    @staticmethod
    def public_status(camera):
        return {"id": camera["id"], "name": camera["name"], "role": camera["role"],
                "configured": bool(camera.get("url")), "online": False, "settings": None, "motion": None}

    def poll_camera(self, camera):
        result = self.public_status(camera)
        if not result["configured"]:
            return result
        try:
            _, _, body = device_request(camera, "/set")
            settings = json.loads(body)
            if not isinstance(settings, dict):
                raise ValueError("无效设备状态")
            result.update(online=True, settings=settings)
            if settings.get("role") != camera["role"]:
                result["warning"] = "设备固件角色与配置不一致"
            if camera["role"] == "detector":
                _, _, body = device_request(camera, "/motion")
                motion = json.loads(body)
                if not isinstance(motion, dict):
                    raise ValueError("无效检测状态")
                result["motion"] = motion
        except (OSError, URLError, ValueError, TimeoutError):
            result.update(online=False, error="连接失败，请检查设备地址、账号及局域网连接")
        return result

    def poll(self):
        while not self.stop.is_set():
            with self.lock:
                active = self.last_viewed > 0 and time.monotonic() - self.last_viewed < 30
            if active:
                # One polling thread, no executor or image work while idle.
                results = [self.poll_camera(camera) for camera in self.cameras.values()]
                with self.lock:
                    self.status = results
            self.stop.wait(self.poll_interval if active else 1)

    def snapshot(self):
        with self.lock:
            self.last_viewed = time.monotonic()
            return {"cameras": list(self.status), "events": self.store.snapshot()[:5],
                    "preview_fps": self.preview_fps, "status_poll_seconds": self.poll_interval}


def handler_for(state):
    class Handler(BaseHTTPRequestHandler):
        server_version = "CameraConsole/1.0"

        def send(self, status, data=b"", content_type="application/json; charset=utf-8", headers=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "same-origin")
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            self.end_headers()
            if data:
                self.wfile.write(data)

        def json(self, status, data, headers=None):
            self.send(status, json.dumps(data, ensure_ascii=False).encode("utf-8"), headers=headers)

        def authenticate(self):
            password = state.config.get("password", "")
            if not password:
                return True  # main() only permits this on loopback.
            expected = "Basic " + base64.b64encode((state.config.get("username", "admin") + ":" + password).encode("utf-8")).decode("ascii")
            if hmac.compare_digest(self.headers.get("Authorization", "").encode(), expected.encode()):
                return True
            self.json(401, {"error": "需要登录控制台"}, {"WWW-Authenticate": 'Basic realm="Camera Console", charset="UTF-8"'})
            return False

        def do_GET(self):
            if not self.authenticate():
                return
            path = urlsplit(self.path).path
            if path == "/":
                self.send(200, (ROOT / "web/index.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/api/state":
                self.json(200, state.snapshot())
            elif re.fullmatch(r"/api/events/[a-f0-9]{32}\.jpg", path):
                photo = state.store.photo(path.rsplit("/", 1)[1][:-4])
                self.send(200, photo, "image/jpeg") if photo else self.json(404, {"error": "抓拍不存在"})
            else:
                match = re.fullmatch(r"/api/cameras/(stream|detector)/(snap|settings|photo)", path)
                if not match:
                    self.json(404, {"error": "接口不存在"})
                    return
                identity, action = match.groups()
                camera = state.cameras[identity]
                if not camera.get("url"):
                    self.json(503, {"error": "设备未配置"})
                    return
                query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                suffix = ""
                if action == "snap" and query:
                    value = query.get("after", [""])[0]
                    if set(query) != {"after"} or not re.fullmatch(r"\d{1,10}", value):
                        self.json(400, {"error": "无效帧序号"})
                        return
                    suffix = "?after=" + value
                elif action == "settings" and query:
                    if set(query) == {"size"} and query["size"][0] in ("qvga", "cif", "hvga", "vga", "svga"):
                        suffix = "?size=" + query["size"][0]
                    else:
                        self.json(400, {"error": "无效分辨率"})
                        return
                upstream = {"snap": "/snap", "settings": "/set", "photo": "/motion/photo"}[action]
                if not state.preview_slot.acquire(blocking=False):
                    self.json(429, {"error": "画面通道繁忙，请稍后重试"}, {"Retry-After": "1"})
                    return
                try:
                    status, headers, body = device_request(camera, upstream + suffix)
                    forwarded = {"X-Frame-Id": headers["X-Frame-Id"]} if "X-Frame-Id" in headers else {}
                    self.send(status, body, "application/json; charset=utf-8" if action == "settings" else "image/jpeg", forwarded)
                except (OSError, URLError, ValueError, TimeoutError):
                    self.json(502, {"error": "设备连接失败"})
                finally:
                    state.preview_slot.release()

        def do_POST(self):
            # Webhook has a separate credential; never needs a browser session.
            if self.path != "/api/events":
                self.json(404, {"error": "接口不存在"})
                return
            token = state.config.get("webhook_token", "")
            if not token or token.startswith("CHANGE_ME"):
                self.json(503, {"error": "请先配置 Webhook 令牌"})
                return
            if not hmac.compare_digest(self.headers.get("Authorization", "").encode(), ("Bearer " + token).encode()):
                self.json(401, {"error": "令牌无效"})
                return
            if self.headers.get("Content-Type", "").split(";", 1)[0].strip() != "image/jpeg":
                self.json(415, {"error": "需要 image/jpeg"})
                return
            if not state.upload_slot.acquire(blocking=False):
                self.json(429, {"error": "抓拍接收繁忙"}, {"Retry-After": "1"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                score = int(self.headers.get("X-Motion-Score", "0"))
                event = int(self.headers.get("X-Camera-Event", "0"))
                uptime = int(self.headers.get("X-Camera-Uptime-Ms", "0"))
                if not 0 < length <= MAX_JPEG:
                    self.json(413, {"error": "照片大小超出限制"})
                    return
                if not 0 <= score <= 100 or not 0 <= event <= 0xFFFFFFFF or not 0 <= uptime <= 0xFFFFFFFF:
                    raise ValueError("无效事件信息")
                self.connection.settimeout(5)
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError("照片传输不完整")
                item = state.store.add(body, score, event, uptime)
                self.json(201, item)
            except (ValueError, TimeoutError):
                self.json(400, {"error": "无效事件或 JPEG"})
            except OSError:
                self.json(500, {"error": "抓拍保存失败"})
            finally:
                state.upload_slot.release()

        def log_message(self, fmt, *args):
            # Do not print query strings, auth headers or credentials.
            if args and args[0].startswith("POST "):
                print("[event]", args[1], flush=True)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("config.local.json"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--data-dir", type=Path, default=Path(__file__).with_name("data"))
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8")) if args.config.exists() else {}
    if args.host not in ("127.0.0.1", "::1", "localhost") and len(config.get("password", "")) < 8:
        parser.error("局域网监听需要先设置至少 8 位的控制台密码")
    state = ConsoleState(config, args.data_dir)
    server = LimitedHTTPServer((args.host, args.port), handler_for(state))
    worker = threading.Thread(target=state.poll, daemon=True)
    worker.start()
    print(f"Camera console: http://{args.host}:{args.port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.stop.set()
        server.server_close()
        worker.join(timeout=6)


if __name__ == "__main__":
    main()
