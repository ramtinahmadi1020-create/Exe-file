#!/usr/bin/env python3
import hashlib
import os
import struct
import sys
import zlib


def build_installer(stub_path: str, files_spec: list[tuple[str, str]], out_path: str) -> None:
    with open(stub_path, "rb") as f:
        stub_bytes = f.read()

    # Align payload start to 16-byte boundary
    pad_len = (16 - (len(stub_bytes) % 16)) % 16
    prefix = stub_bytes + (b"\x00" * pad_len)
    payload_offset = len(prefix)

    chunks = [struct.pack("<I", len(files_spec))]
    for arc_name, src_path in files_spec:
        with open(src_path, "rb") as f:
            raw = f.read()
        comp = zlib.compress(raw, level=9)
        name_bytes = arc_name.encode("utf-8")
        entry_hdr = struct.pack("<I", len(name_bytes)) + name_bytes + struct.pack("<QQ", len(raw), len(comp))
        chunks.append(entry_hdr)
        chunks.append(comp)
        print(f"  Packed {arc_name:24s}: {len(raw):11,d} -> {len(comp):11,d} bytes")

    archive_blob = b"".join(chunks)
    digest = hashlib.sha256(archive_blob).digest()
    footer = struct.pack("<QQ32s8s", payload_offset, len(archive_blob), digest, b"VPNECHO1")

    with open(out_path, "wb") as f:
        f.write(prefix)
        f.write(archive_blob)
        f.write(footer)

    total_sz = os.path.getsize(out_path)
    print(f"Created {out_path}: {total_sz:,} bytes ({total_sz / (1024 * 1024):.2f} MiB)")


if __name__ == "__main__":
    repo = "/home/user/Exe-file"
    files = [
        ("VPN Echo.exe",         "/tmp/win-out/VPN Echo.exe"),
        ("sing-box.exe",         "/tmp/sing-box-build/sing-box.exe"),
        ("libcronet.dll",        os.path.join(repo, "Core/libcronet.dll")),
        ("servers.json",         os.path.join(repo, "Assets/servers.json")),
        ("echo.ico",             os.path.join(repo, "Assets/echo.ico")),
        ("sing-box-LICENSE.txt", os.path.join(repo, "Core/LICENSE")),
        ("GPL-3.0.txt",          os.path.join(repo, "Licenses/GPL-3.0.txt")),
        ("Uninstall.exe",        "/tmp/win-out/Uninstall.exe"),
    ]
    build_installer(
        "/tmp/win-out/VPN-Echo-Setup-stub.exe",
        files,
        "/tmp/win-out/VPN-Echo-Setup.exe",
    )
