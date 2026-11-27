#!/usr/bin/env bash
# Baixa logs do Xbox 360 via FTP da Aurora (FtpDll).
# Uso: ./scripts/pull-console-logs.sh [IP]
# Defaults: IP=192.168.2.184  user=xboxftp  pass=xboxftp  porta=21  volume=Usb0
set -euo pipefail
IP="${1:-192.168.2.184}"
OUT="build/console-logs-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PYTHONUNBUFFERED=1 python3 -u - <<PY
import socket, re, pathlib
IP = "${IP}"
USER, PASS = "xboxftp", "xboxftp"
OUT = pathlib.Path("${OUT}")
OUT.mkdir(parents=True, exist_ok=True)

def recv_all(s, wait=0.3):
    s.settimeout(2.5)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            s.settimeout(wait)
    except Exception:
        pass
    return data.decode("latin1", "replace")

def cmd(s, line):
    print(">>", line, flush=True)
    s.sendall((line + "\r\n").encode())
    r = recv_all(s)
    print("<<", r.strip()[:300], flush=True)
    return r

def pasv(s):
    r = cmd(s, "PASV")
    a, b, c, d, p1, p2 = map(int, re.search(r"(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)", r).groups())
    return socket.create_connection((f"{a}.{b}.{c}.{d}", p1 * 256 + p2), timeout=10)

def retr(s, remote, local):
    ds = pasv(s)
    r = cmd(s, f"RETR {remote}")
    if "150" not in r and "125" not in r:
        ds.close()
        return False
    data = b""
    ds.settimeout(60)
    try:
        while True:
            c = ds.recv(65536)
            if not c:
                break
            data += c
    except Exception as e:
        print("retr data", e, flush=True)
    ds.close()
    print(recv_all(s).strip()[:80], flush=True)
    local.write_bytes(data)
    print(f"SAVED {local} {len(data)}", flush=True)
    return True

s = socket.create_connection((IP, 21), timeout=5)
print(recv_all(s).strip(), flush=True)
cmd(s, f"USER {USER}")
cmd(s, f"PASS {PASS}")
cmd(s, "CWD Usb0")
for f in ["bt360_trace.log", "bt360_diag.log", "launch.ini", "bt360_scan.bin"]:
    retr(s, f, OUT / f)
cmd(s, "CWD Apps/BTManager")
retr(s, "btmanager.log", OUT / "btmanager.log")
cmd(s, "QUIT")
print("ok", OUT, flush=True)
PY
ls -la "$OUT"
