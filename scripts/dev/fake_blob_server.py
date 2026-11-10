#!/usr/bin/env python3
"""A loopback stand-in for the relay's files routes, for the manual large-attachment run.

Serves what `HttpBlobClient` (upload) and `AttachmentFetch` (download) call, as
docs/contracts/SERVICE_ENDPOINTS.md (relay files) describes them:

  POST /v1/files/start      { blob_id, mode, part_bytes, part_count, upload_url?, expires_in, pending_expires_at }
  POST /v1/files/part       { upload_url, part_bytes, expires_in }
  PUT  /storage/<id>[/<n>]  the ciphertext (single) or part n (multipart)
  POST /v1/files/complete   { blob_id, token, share_path, expires_at }, once
  POST /v1/files/delete     { success: true }
  POST /v1/files/list       { files: [...], usage: {...} }
  GET  /api/relay/v1/files/<id>/download?t=<token>   302 to /storage/<id>, then the bytes

The `/v1/...` routes are also answered under `/api/relay` (the real relay base URL ends in it). Signatures are
not checked; nothing is retained beyond this process (blobs live in --dir, a temp dir by default). Single PUT up
to 16 MiB, multipart above it with 16 MiB parts, as the relay plans them.

  python3 scripts/dev/fake_blob_server.py --port 8767
"""

import argparse
import json
import os
import re
import secrets
import shutil
import signal
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

PART_BYTES = 16 * 1024 * 1024
CHUNK = 1024 * 1024


class Blobs:
    def __init__(self, directory):
        self.dir = directory
        self.meta = {}  # blob_id -> {byte_length, part_count, status, token}

    def path(self, blob_id, part=None):
        name = blob_id if part is None else f"{blob_id}.part{part}"
        return os.path.join(self.dir, name)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # answers `Expect: 100-continue` on a large PUT
    blobs = None

    def log_message(self, fmt, *args):
        # No query strings: the download token is a credential.
        print("fake_blob_server: " + re.sub(r"\?[^ \"]*", "", fmt % args), flush=True)

    def reply(self, status, body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else b""
        self.send_response(status)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def fail(self, status, error, code=None):
        body = {"error": error}
        if code:
            body["code"] = code
        self.reply(status, body)

    def origin(self):
        return f"http://{self.headers.get('Host')}"

    def route(self):
        path = urlsplit(self.path).path
        return path[len("/api/relay"):] if path.startswith("/api/relay/") else path

    def read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        try:
            return json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return None

    def do_POST(self):
        body = self.read_json()
        if body is None:
            return self.fail(400, "Invalid JSON")
        handlers = {
            "/v1/files/start": self.start,
            "/v1/files/part": self.part,
            "/v1/files/complete": self.complete,
            "/v1/files/delete": self.delete,
            "/v1/files/list": self.list,
        }
        handler = handlers.get(self.route())
        if not handler:
            return self.fail(404, "Not found")
        return handler(body)

    def start(self, body):
        size = int(body.get("byte_length") or 0)
        if size <= 0:
            return self.fail(400, "Invalid byte_length", "INVALID_BYTE_LENGTH")
        blob_id = secrets.token_hex(8)
        multipart = size > PART_BYTES
        part_count = (size + PART_BYTES - 1) // PART_BYTES if multipart else 1
        self.blobs.meta[blob_id] = {"byte_length": size, "part_count": part_count, "status": "pending"}
        answer = {
            "blob_id": blob_id,
            "mode": "multipart" if multipart else "single",
            "part_bytes": PART_BYTES if multipart else size,
            "part_count": part_count,
            "expires_in": 3600,
            "pending_expires_at": "2099-01-01T00:00:00.000Z",
        }
        if not multipart:
            answer["upload_url"] = f"{self.origin()}/storage/{blob_id}"
        self.reply(200, answer)

    def part(self, body):
        meta = self.blobs.meta.get(body.get("blob_id"))
        if not meta or meta["status"] != "pending":
            return self.fail(400, "Blob is not pending", "BLOB_NOT_PENDING")
        number = int(body.get("part_number") or 0)
        if not 1 <= number <= meta["part_count"]:
            return self.fail(400, "Invalid part number", "INVALID_PART_NUMBER")
        last = meta["byte_length"] - (meta["part_count"] - 1) * PART_BYTES
        self.reply(200, {
            "upload_url": f"{self.origin()}/storage/{body['blob_id']}/{number}",
            "part_bytes": last if number == meta["part_count"] else PART_BYTES,
            "expires_in": 3600,
        })

    def complete(self, body):
        blob_id = body.get("blob_id")
        meta = self.blobs.meta.get(blob_id)
        if not meta or meta["status"] != "pending":
            return self.fail(400, "Blob is not pending", "BLOB_NOT_PENDING")
        if meta["part_count"] > 1:
            parts = [self.blobs.path(blob_id, n) for n in range(1, meta["part_count"] + 1)]
            if not all(os.path.exists(p) for p in parts):
                return self.fail(400, "Upload incomplete", "UPLOAD_INCOMPLETE")
            with open(self.blobs.path(blob_id), "wb") as out:
                for p in parts:
                    with open(p, "rb") as part:
                        shutil.copyfileobj(part, out, CHUNK)
                    os.remove(p)
        if not os.path.exists(self.blobs.path(blob_id)) or \
                os.path.getsize(self.blobs.path(blob_id)) != meta["byte_length"]:
            return self.fail(400, "Upload incomplete", "UPLOAD_INCOMPLETE")
        meta["status"] = "complete"
        meta["token"] = secrets.token_urlsafe(16)
        self.reply(200, {
            "blob_id": blob_id,
            "token": meta["token"],
            "share_path": f"/api/relay/v1/files/{blob_id}/download?t={meta['token']}",
            "expires_at": "2099-01-01T00:00:00.000Z",
        })

    def delete(self, body):
        blob_id = body.get("blob_id")
        if self.blobs.meta.pop(blob_id, None) is None:
            return self.fail(404, "Not found", "BLOB_NOT_FOUND")
        for name in os.listdir(self.blobs.dir):
            if name == blob_id or name.startswith(blob_id + ".part"):
                os.remove(os.path.join(self.blobs.dir, name))
        self.reply(200, {"success": True})

    def list(self, body):
        wanted = body.get("status") or ""
        files = [{"blob_id": blob_id, "status": meta["status"], "byte_length": meta["byte_length"]}
                 for blob_id, meta in self.blobs.meta.items() if not wanted or meta["status"] == wanted]
        used = sum(meta["byte_length"] for meta in self.blobs.meta.values())
        pending = sum(1 for meta in self.blobs.meta.values() if meta["status"] == "pending")
        self.reply(200, {"files": files, "usage": {"bytes_used": used, "pending_count": pending,
                                                   "quota_bytes": 1 << 30, "max_pending": 20}})

    def do_PUT(self):
        match = re.fullmatch(r"/storage/([0-9a-f]+)(?:/(\d+))?", urlsplit(self.path).path)
        meta = self.blobs.meta.get(match.group(1)) if match else None
        if not meta or meta["status"] != "pending":
            return self.fail(403, "Forbidden")
        part = int(match.group(2)) if match.group(2) else None
        left = int(self.headers.get("Content-Length", "0"))
        with open(self.blobs.path(match.group(1), part), "wb") as out:
            while left > 0:
                chunk = self.rfile.read(min(CHUNK, left))
                if not chunk:
                    break
                out.write(chunk)
                left -= len(chunk)
        self.reply(200)

    def do_GET(self):
        url = urlsplit(self.path)
        download = re.fullmatch(r"/api/relay/v1/files/([0-9a-f]+)/download", url.path)
        if download:
            meta = self.blobs.meta.get(download.group(1))
            token = parse_qs(url.query).get("t", [""])[0]
            if not meta or meta["status"] != "complete" or token != meta.get("token"):
                return self.fail(404, "Not found")
            return self.reply(302, headers={"Location": f"{self.origin()}/storage/{download.group(1)}",
                                            "Cache-Control": "no-store"})
        stored = re.fullmatch(r"/storage/([0-9a-f]+)", url.path)
        meta = self.blobs.meta.get(stored.group(1)) if stored else None
        if not meta or meta["status"] != "complete":
            return self.fail(404, "Not found")
        path = self.blobs.path(stored.group(1))
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(os.path.getsize(path)))
        self.end_headers()
        with open(path, "rb") as source:
            shutil.copyfileobj(source, self.wfile, CHUNK)


def stop(*_):
    raise KeyboardInterrupt  # a kill removes the temp dir too


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--dir", help="where blobs are kept (default: a temp dir, removed on exit)")
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    directory = args.dir or tempfile.mkdtemp(prefix="fake-blob-")
    os.makedirs(directory, exist_ok=True)
    Handler.blobs = Blobs(directory)
    print(f"fake_blob_server: http://{args.host}:{args.port} blobs in {directory}", flush=True)
    signal.signal(signal.SIGTERM, stop)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if not args.dir:
            shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    main()
