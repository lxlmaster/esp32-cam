import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

spec = importlib.util.spec_from_file_location("camera_console", Path(__file__).resolve().parents[1] / "console/server.py")
console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console)
JPEG = b"\xff\xd8test-image\xff\xd9"


class FakeCamera(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get("Authorization") != "Basic " + base64.b64encode(b"cam:device-password").decode():
            self.send_response(401)
            self.end_headers()
            return
        if self.path.startswith("/snap"):
            self.send_response(204 if self.path == "/snap?after=7" else 200)
            self.send_header("X-Frame-Id", "7")
            self.end_headers()
            if self.path != "/snap?after=7":
                self.wfile.write(JPEG)
            return
        if self.path == "/set":
            body = {"role": "stream", "size": "400x296", "quality": 75, "fps_x10": 80}
        else:
            body = {"enabled": True, "events": 0}
        self.send_response(200)
        self.end_headers()
        self.wfile.write(json.dumps(body).encode())

    def log_message(self, *args):
        pass


class ConsoleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.camera = ThreadingHTTPServer(("127.0.0.1", 0), FakeCamera)
        cls.camera_thread = threading.Thread(target=cls.camera.serve_forever, daemon=True)
        cls.camera_thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.camera.shutdown()
        cls.camera.server_close()
        cls.camera_thread.join()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        config = {"username": "admin", "password": "console-password", "webhook_token": "secret-token",
                  "retain_events": 2,
                  "stream": {"url": "http://127.0.0.1:" + str(self.camera.server_port), "username": "cam", "password": "device-password"}}
        self.state = console.ConsoleState(config, self.directory.name)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), console.handler_for(self.state))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:" + str(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def request(self, path, data=None, auth=True, headers=None):
        request_headers = dict(headers or {})
        if auth:
            request_headers["Authorization"] = "Basic " + base64.b64encode(b"admin:console-password").decode()
        request = Request(self.base + path, data=data, headers=request_headers)
        try:
            with urlopen(request, timeout=3) as response:
                return response.status, response.headers, response.read()
        except HTTPError as error:
            return error.code, error.headers, error.read()

    def event(self, data=JPEG, score="24", token="secret-token"):
        return self.request("/api/events", data=data, auth=False,
                            headers={"Authorization": "Bearer " + token, "Content-Type": "image/jpeg", "X-Motion-Score": score})

    def test_console_auth_and_no_secret_leaks(self):
        self.assertEqual(self.request("/api/state", auth=False)[0], 401)
        status, _, body = self.request("/api/state")
        self.assertEqual(status, 200)
        self.assertNotIn(b"device-password", body)
        self.assertNotIn(b"secret-token", body)

    def test_webhook_auth_and_invalid_inputs(self):
        self.assertEqual(self.event(token="wrong")[0], 401)
        self.assertEqual(self.event(data=b"broken")[0], 400)
        self.assertEqual(self.event(score="101")[0], 400)
        self.assertEqual(self.event(data=b"x" * (console.MAX_JPEG + 1))[0], 413)
        self.assertEqual(self.state.store.snapshot(), [])

    def test_event_photo_and_retention_persist(self):
        identities = []
        for _ in range(3):
            status, _, body = self.event()
            self.assertEqual(status, 201)
            identities.append(json.loads(body)["id"])
        self.assertEqual(len(self.state.store.snapshot()), 2)
        self.assertEqual(self.request("/api/events/" + identities[0] + ".jpg")[0], 404)
        self.assertEqual(self.request("/api/events/" + identities[2] + ".jpg")[2], JPEG)
        restarted = console.EventStore(self.directory.name, 2)
        self.assertEqual([x["id"] for x in restarted.snapshot()], list(reversed(identities[1:])))

    def test_snapshot_proxy_and_no_duplicate_frames(self):
        status, headers, body = self.request("/api/cameras/stream/snap")
        self.assertEqual((status, headers["X-Frame-Id"], body), (200, "7", JPEG))
        self.assertEqual(self.request("/api/cameras/stream/snap?after=7")[0], 204)
        self.assertEqual(self.request("/api/cameras/detector/snap")[0], 503)

    def test_allowed_routes_and_settings(self):
        self.assertEqual(self.request("/api/cameras/stream/settings?reg=0x51")[0], 400)
        self.assertEqual(self.request("/api/cameras/stream/settings?size=invalid")[0], 400)
        self.assertEqual(self.request("/api/cameras/stream/snap?after=x")[0], 400)
        self.assertEqual(self.request("/api/cameras/other/snap")[0], 404)
        self.assertEqual(self.request("/api/events/../../config.local.json")[0], 404)

    def test_status_polling_and_auth_failure(self):
        camera = self.state.cameras["stream"]
        self.assertTrue(self.state.poll_camera(camera)["online"])
        self.assertFalse(self.state.poll_camera({**camera, "password": "wrong"})["online"])

    def test_device_url_validation(self):
        for url in ("file:///etc/passwd", "http://user:pass@127.0.0.1", "http://127.0.0.1/other", "http://127.0.0.1?x=1"):
            with self.assertRaises(ValueError):
                console.device_request({"url": url}, "/set")

    def test_missing_webhook_token_rejects_uploads(self):
        self.state.config["webhook_token"] = ""
        self.assertEqual(self.event()[0], 503)

    def test_busy_preview_does_not_block_events_or_state(self):
        self.state.preview_slot.acquire()
        try:
            self.assertEqual(self.request("/api/cameras/stream/snap")[0], 429)
            self.assertEqual(self.event()[0], 201)
            self.assertEqual(self.request("/api/state")[0], 200)
        finally:
            self.state.preview_slot.release()

    def test_one_upload_at_a_time_and_recovery(self):
        self.state.upload_slot.acquire()
        try:
            self.assertEqual(self.event()[0], 429)
        finally:
            self.state.upload_slot.release()
        self.assertEqual(self.event()[0], 201)

    def test_low_memory_defaults_and_bounded_state(self):
        self.assertEqual(self.state.preview_fps, 5)
        self.assertEqual(self.state.poll_interval, 10)
        self.assertEqual(self.state.last_viewed, 0)
        state = self.state.snapshot()
        self.assertGreater(self.state.last_viewed, 0)
        self.assertEqual(state["preview_fps"], 5)
        self.assertLessEqual(len(state["events"]), 5)

    def test_server_admission_is_bounded(self):
        server = console.LimitedHTTPServer(("127.0.0.1", 0), console.handler_for(self.state), max_clients=1)
        import socket
        client, incoming = socket.socketpair()
        server.slots.acquire()
        try:
            server.process_request(incoming, ("127.0.0.1", 0))
            client.settimeout(1)
            self.assertEqual(client.recv(1), b"")
            self.assertFalse(server.slots.acquire(blocking=False))
        finally:
            server.slots.release()
            client.close()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
