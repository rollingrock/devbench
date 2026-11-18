"""A fake devbench for the client's unit tests: a local HTTP server that answers from scripted responses, no game."""

from __future__ import annotations

import base64
import json
import struct
import threading
import zlib
from collections.abc import Callable
from fnmatch import fnmatchcase
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlsplit

# A scripted tool: takes the call's arguments, returns the answer (200) or (status, answer). An answer of bytes is
# sent as is, as text/plain, to stand for a body that is not JSON.
ToolHandler = Callable[[dict[str, Any]], Any]

# A tool handler returning this, or health set to it, closes the connection without answering, as a game dying
# mid-call does.
HANG_UP = object()


class FakeDevbench:
    """
    Answers /api/tool/<name> and the /api/tools listing from tools, /api/health from health and
    /api/events?since=<seq> from events, on port of 127.0.0.1 (0: a free one) with HTTP/1.1 keep-alive. calls records
    each request as (method, path, body); connections counts the TCP connections accepted. drop_next() closes the
    socket after the next answer, as a restarting game does; stop() stops listening, as a game that quit.
    """

    def __init__(self, port: int = 0) -> None:
        self.tools: dict[str, ToolHandler] = {}
        self.health: Any = {"ok": True}
        self.events: list[dict[str, Any]] = []
        self.calls: list[tuple[str, str, Any]] = []
        self.connections = 0
        self._drop = False
        self._stopped = False
        self._lock = threading.Lock()
        handler = type("Handler", (_Handler,), {"fake": self})
        self._server = ThreadingHTTPServer(("127.0.0.1", port), handler)
        self._server.daemon_threads = True
        self.port: int = self._server.server_address[1]
        # a short poll so close() (shutdown) does not wait out the default 0.5 s per test
        self._thread = threading.Thread(target=self._server.serve_forever, args=(0.02,), daemon=True)
        self._thread.start()

    def publish(self, topic: str, data: Any = None) -> dict[str, Any]:
        """Append an event with the next seq after the last one, as devbench's EventBus does, and return it."""
        with self._lock:
            seq = self.events[-1]["seq"] + 1 if self.events else 1
            event = {"seq": seq, "frame": 0, "topic": topic, "data": data}
            self.events.append(event)
            return event

    def drop_next(self) -> None:
        """Close the kept-alive socket right after the next answer, without telling the client."""
        with self._lock:
            self._drop = True

    def stop(self) -> None:
        """
        Stop listening, as a game that quit: new connections are refused. Safe to call again, and from a tool
        handler (shutdown waits for the serving thread, not the handler's).
        """
        with self._lock:
            if self._stopped:
                return
            self._stopped = True
        self._server.shutdown()
        self._server.server_close()

    def close(self) -> None:
        self.stop()
        self._thread.join()

    def _take_drop(self) -> bool:
        with self._lock:
            drop, self._drop = self._drop, False
            return drop

    def _count_connection(self) -> None:
        with self._lock:
            self.connections += 1

    def _record(self, method: str, path: str, body: Any) -> None:
        with self._lock:
            self.calls.append((method, path, body))


class _Handler(BaseHTTPRequestHandler):
    """One connection to the fake; fake is set on the subclass FakeDevbench makes."""

    protocol_version = "HTTP/1.1"
    fake: FakeDevbench

    def setup(self) -> None:
        super().setup()
        self.fake._count_connection()

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A002 - the base class names it so
        pass

    def do_GET(self) -> None:
        url = urlsplit(self.path)
        self.fake._record("GET", self.path, None)
        if url.path == "/api/health":
            if self.fake.health is HANG_UP:
                self.close_connection = True
            else:
                self._answer(self.fake.health)
        elif url.path == "/api/tools":
            names = sorted(self.fake.tools)
            self._answer({"tools": [{"name": name, "description": "", "inputSchema": {}} for name in names]})
        elif url.path == "/api/events":
            since = int(parse_qs(url.query).get("since", ["0"])[0])
            with self.fake._lock:
                events = list(self.fake.events)
            head = events[-1]["seq"] if events else 0
            self._answer({"headSeq": head, "events": [event for event in events if event["seq"] > since]})
        else:
            self._answer((404, {"ok": False, "error": f"no route {url.path}"}))

    def do_POST(self) -> None:
        url = urlsplit(self.path)
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        self.fake._record("POST", self.path, body)
        name = url.path.removeprefix("/api/tool/")
        handler = self.fake.tools.get(name) if url.path.startswith("/api/tool/") else None
        if handler is None:
            self._answer((404, {"ok": False, "error": f"unknown tool {name}"}))
        else:
            result = handler(body)
            if result is HANG_UP:
                self.close_connection = True
            else:
                self._answer(result)

    def _answer(self, result: Any) -> None:
        """Send a scripted result: an answer (200) or (status, answer); bytes go out as plain text."""
        status, answer = result if isinstance(result, tuple) else (200, result)
        if isinstance(answer, bytes):
            data, content_type = answer, "text/plain"
        else:
            data, content_type = json.dumps(answer).encode("utf-8"), "application/json"
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()
        if self.fake._take_drop():
            self.close_connection = True


def _glob_match(pattern, segments):
    """Whether path segments match glob segments: '*' and '?' within a name, '**' for any folders, case ignored."""
    if not pattern:
        return not segments
    if pattern[0] == "**":
        return any(_glob_match(pattern[1:], segments[i:]) for i in range(len(segments) + 1))
    return (
        bool(segments)
        and fnmatchcase(segments[0].lower(), pattern[0].lower())
        and _glob_match(pattern[1:], segments[1:])
    )


class FakeFiles:
    """
    A scripted files tool over an in-memory file set: files maps (root, path) to bytes. writes and deletes record the
    paths each changed; listing sets the flags a listing that is not whole carries.
    """

    def __init__(self, files=None):
        self.files = dict(files or {})
        self.writes = []
        self.deletes = []
        self.listing = {}

    def _find(self, root, path):
        """The stored key of a path, case ignored, or None."""
        wanted = (root, path.lower())
        return next((key for key in self.files if (key[0], key[1].lower()) == wanted), None)

    def __call__(self, args):
        action, root, path = args["action"], args["root"], args["path"]
        if action == "list":
            pattern = path.split("/")
            listed = [
                {"path": p, "bytes": len(data), "modifiedMs": 0}
                for (r, p), data in sorted(self.files.items())
                if r == root and _glob_match(pattern, p.split("/"))
            ]
            return {"ok": True, "count": len(listed), "files": listed, **self.listing}
        key = self._find(root, path)
        if action == "read":
            if key is None:
                return (404, {"ok": False, "error": f"files: no file '{path}' under {root}"})
            assert args["encoding"] == "base64"
            return {"ok": True, "path": path, "content": base64.b64encode(self.files[key]).decode("ascii")}
        if action == "write":
            assert args["encoding"] == "base64"
            self.files[key or (root, path)] = base64.b64decode(args["content"])
            self.writes.append(f"{root}/{path}")
            return {"ok": True, "created": key is None}
        if action == "delete":
            if key is None:
                return {"ok": True, "deleted": False}
            del self.files[key]
            self.deletes.append(f"{root}/{path}")
            return {"ok": True, "deleted": True}
        raise AssertionError(f"unexpected files action {action}")


def make_bmp(width, height, pixel, bits=24, top_down=False):
    """
    An uncompressed BMP as devbench writes a dump: pixel(x, y) gives each stored pixel's 3 bytes (B, G, R), row y in
    file order (bottom-up unless top_down, which stores a negative height), rows padded to 4 bytes.
    """
    stride = (width * 3 + 3) & ~3
    rows = b"".join(b"".join(pixel(x, y) for x in range(width)) + bytes(stride - width * 3) for y in range(height))
    stored_height = -height if top_down else height
    info = struct.pack("<IiiHHIIiiII", 40, width, stored_height, 1, bits, 0, len(rows), 2835, 2835, 0, 0)
    return b"BM" + struct.pack("<IHHI", 54 + len(rows), 0, 0, 54) + info + rows


def read_png(data):
    """
    The width, height and pixels (rows top-down of (R, G, B)) of an 8-bit RGB PNG whose scanlines are all filter 0,
    as bmp_to_png writes them; asserts the format and each chunk's CRC.
    """
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    chunks, at = {}, 8
    while at < len(data):
        (length,) = struct.unpack_from(">I", data, at)
        kind, body = data[at + 4 : at + 8], data[at + 8 : at + 8 + length]
        assert struct.unpack_from(">I", data, at + 8 + length)[0] == zlib.crc32(kind + body)
        chunks[kind] = chunks.get(kind, b"") + body
        at += 12 + length
    width, height, depth, color = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    assert (depth, color) == (8, 2) and b"IEND" in chunks
    raw = zlib.decompress(chunks[b"IDAT"])
    line = 1 + width * 3
    assert all(raw[row * line] == 0 for row in range(height))
    pixels = [
        [tuple(raw[row * line + 1 + x * 3 : row * line + 4 + x * 3]) for x in range(width)] for row in range(height)
    ]
    return width, height, pixels
