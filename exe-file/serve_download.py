#!/usr/bin/env python3
"""Minimal RFC 7233-capable static file server for the VPN Echo release zip.

Serves exactly one file as an attachment, supports HTTP Range requests
(so interrupted downloads can resume), and exposes a small index page with
size / sha256 so the download can be verified.
"""
import hashlib
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.abspath(__file__))
FILE = os.path.join(ROOT, "VPN-Echo-Windows-x64.zip")
NAME = "VPN-Echo-Windows-x64.zip"
PORT = int(os.environ.get("PORT", "8080"))
CHUNK = 1024 * 100


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(blk)
    return h.hexdigest()


SIZE = os.path.getsize(FILE)
SHA = sha256_of(FILE)

INDEX = f"""<!doctype html>
<html lang="fa" dir="rtl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VPN Echo 1.1.1.1 - دانلود</title>
<style>
  :root {{ color-scheme: dark; }}
  body {{ margin:0; min-height:100vh; display:flex; align-items:center; justify-content:center;
         background:#111315; color:#d2d6d8;
         font-family:system-ui,-apple-system,"Segoe UI",Tahoma,sans-serif; padding:24px; }}
  .card {{ width:100%; max-width:620px; background:#1d2124; border:1px solid #303639;
          border-radius:16px; padding:32px; box-sizing:border-box; }}
  h1 {{ margin:0 0 6px; font-size:22px; font-weight:700; letter-spacing:.2px; }}
  .ver {{ color:#66c995; font-size:13px; margin-bottom:22px; }}
  a.dl {{ display:block; text-align:center; text-decoration:none; background:#66c995;
         color:#0b0f10; font-weight:700; padding:16px; border-radius:11px; font-size:16px; }}
  a.dl:hover {{ background:#7ad6a4; }}
  .meta {{ margin-top:22px; font-size:12.5px; line-height:1.9; color:#899196; }}
  .meta code {{ color:#d2d6d8; background:#111315; padding:2px 6px; border-radius:5px;
               font-size:11.5px; word-break:break-all; direction:ltr; display:inline-block; }}
  ul {{ margin:14px 0 0; padding-inline-start:18px; font-size:13px; color:#b6bcc0; line-height:1.9; }}
  .rule {{ margin-top:20px; padding-top:16px; border-top:1px solid #303639; font-size:12.5px;
          color:#899196; line-height:1.9; }}
</style>
</head>
<body>
<div class="card">
  <h1>VPN Echo</h1>
  <div class="ver">نسخه 1.1.1.1 &nbsp;&middot;&nbsp; Windows 10 / 11 (x64)</div>
  <a class="dl" href="/{NAME}" download>دانلود فایل کامل (ZIP)</a>
  <ul>
    <li><code>VPN-Echo-Setup.exe</code> — نصب‌کننده کامل</li>
    <li>پوشه <code>VPN Echo</code> — نسخه پرتابل (بدون نصب)</li>
    <li><code>README-FA.txt</code> — راهنمای فارسی</li>
  </ul>
  <div class="meta">
    حجم فایل: <code>{SIZE:,} bytes</code> ({SIZE / 1048576:.1f} MB)<br>
    SHA-256: <code>{SHA}</code>
  </div>
  <div class="rule">
    رنگ پینگ در این نسخه: زیر ۱۰۰۰ میلی‌ثانیه سبز، ۱۰۰۰ و بالاتر قرمز،
    بدون پاسخ یا خطا قرمز — رنگ نارنجی حذف شده است.<br>
    برای ساخت کارت شبکه مجازی VPN، برنامه به دسترسی Administrator نیاز دارد و خودش درخواست می‌کند.
  </div>
</div>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep the log readable
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _headers_common(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-store")

    def do_HEAD(self):
        if self.path.split("?")[0] in ("/", "/index.html"):
            body = INDEX.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self._headers_common()
            self.end_headers()
            return
        if self.path.split("?")[0] not in ("/" + NAME, "/" + NAME + "/"):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(SIZE))
        self.send_header("Content-Disposition", f'attachment; filename="{NAME}"')
        self._headers_common()
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            body = INDEX.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self._headers_common()
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/" + NAME + ".sha256":
            body = f"{SHA}  {NAME}\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self._headers_common()
            self.end_headers()
            self.wfile.write(body)
            return

        if path not in ("/" + NAME, "/" + NAME + "/"):
            self.send_error(404)
            return

        start, end = 0, SIZE - 1
        status = 200
        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)$", rng.strip())
            if m:
                if m.group(1) == "" and m.group(2):
                    start = max(0, SIZE - int(m.group(2)))
                elif m.group(1):
                    start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), SIZE - 1)
                if start >= SIZE or start > end:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{SIZE}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                status = 206

        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(length))
        self.send_header("Content-Disposition", f'attachment; filename="{NAME}"')
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{SIZE}")
        self._headers_common()
        self.end_headers()

        with open(FILE, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(CHUNK, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(chunk)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    srv.daemon_threads = True
    sys.stderr.write(f"serving {NAME} ({SIZE:,} bytes) on 0.0.0.0:{PORT}\n")
    srv.serve_forever()
