#!/usr/bin/env python3
"""End-to-end test: load the built .so and feed it events from a fake Umbriel."""

import ctypes
import json
import os
import socket
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "build" / "plugins" / "libumbrielsupport.so"
HELPER = Path(os.environ.get(
    "WL_SHIMEJI_LIBDIR", "/usr/local/lib")) / "libwayland-shimeji-plugins.so"


class FakeUmbriel:
    def __init__(self, path: Path):
        self.path = path
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(path))
        self.listener.listen(1)
        self.stop = threading.Event()
        self.actions = []
        self.client = None
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self.thread.start()

    def _send(self, payload):
        self.client.sendall((json.dumps(payload, separators=(",", ":")) + "\n").encode())

    def _serve(self):
        self.client, _ = self.listener.accept()
        buffer = b""
        while not self.stop.is_set():
            try:
                chunk = self.client.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if not line:
                    continue
                request = json.loads(line)
                if request.get("cmd") == "subscribe":
                    self._send({"event": "outputs", "data": [{
                        "name": "DP-1", "enabled": True, "powered": True,
                        "usable_area": {"x": 0, "y": 0, "width": 1920, "height": 1080},
                    }]})
                    self._send({"event": "windows", "data": [{
                        "id": "win-1", "output": "DP-1", "x": 100, "y": 200,
                        "w": 800, "h": 600, "active": True, "visible": True,
                        "floating": True, "fullscreen": False,
                    }]})
                elif request.get("cmd") == "action":
                    self.actions.append(request)

    def close(self):
        self.stop.set()
        if self.client is not None:
            self.client.close()
        self.listener.close()
        self.thread.join(timeout=2)


SET_ACTIVE_IE = ctypes.CFUNCTYPE(None, ctypes.c_bool, ctypes.c_int32,
                                 ctypes.c_int32, ctypes.c_int32, ctypes.c_int32)
SET_CURSOR = ctypes.CFUNCTYPE(None, ctypes.c_int32, ctypes.c_int32)
HINT = ctypes.CFUNCTYPE(None)


def main():
    assert PLUGIN.is_file(), f"build the plugin first: make"
    tmp = Path(tempfile.mkdtemp(prefix="umbriel-support-test-"))
    server = FakeUmbriel(tmp / "umbriel.sock")
    server.start()
    os.environ["UMBRIEL_SOCKET"] = str(server.path)

    ctypes.CDLL(str(HELPER), mode=ctypes.RTLD_GLOBAL)
    lib = ctypes.CDLL(str(PLUGIN))

    lib.wl_shimeji_plugin_init.argtypes = [
        ctypes.c_void_p, SET_CURSOR, SET_ACTIVE_IE, HINT]
    lib.wl_shimeji_plugin_init.restype = ctypes.c_int
    lib.wl_shimeji_plugin_tick.argtypes = [ctypes.c_void_p]
    lib.wl_shimeji_plugin_move.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.wl_shimeji_plugin_move.restype = ctypes.c_int
    lib.wl_shimeji_plugin_deinit.argtypes = [ctypes.c_void_p]
    lib.wl_shimeji_plugin_deinit.restype = ctypes.c_int

    reported = []

    def on_ie(active, x, y, w, h):
        reported.append((active, x, y, w, h))

    cb_ie = SET_ACTIVE_IE(on_ie)
    cb_cursor = SET_CURSOR(lambda x, y: None)
    cb_hint = HINT(lambda: None)
    handle = ctypes.create_string_buffer(256)

    assert lib.wl_shimeji_plugin_init(ctypes.byref(handle), cb_cursor, cb_ie, cb_hint) == 0

    deadline = time.monotonic() + 3
    while not any(entry[0] for entry in reported) and time.monotonic() < deadline:
        lib.wl_shimeji_plugin_tick(ctypes.byref(handle))
        time.sleep(0.02)
    assert reported, "no window state was reported"
    active_reports = [entry for entry in reported if entry[0]]
    assert active_reports, "no active window was reported"
    assert active_reports[-1] == (True, 100, 200, 800, 600), active_reports

    assert lib.wl_shimeji_plugin_move(ctypes.byref(handle), 500, 400) == 0
    deadline = time.monotonic() + 2
    while not server.actions and time.monotonic() < deadline:
        time.sleep(0.02)
    assert server.actions, "move action was not sent"
    action = server.actions[-1]
    assert action["action"] == "window-move-to", action
    assert action["anchor"] == "top_left", action
    assert action["x"] == 500 and action["y"] == 400, action
    assert action["window"] == "win-1", action

    assert lib.wl_shimeji_plugin_deinit(ctypes.byref(handle)) == 0
    server.close()
    print("plugin integration test passed")


if __name__ == "__main__":
    main()
