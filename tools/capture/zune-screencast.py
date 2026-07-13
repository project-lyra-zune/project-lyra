#!/usr/bin/env python3
"""Live Zune HD screen mirror via the on-device plugin-screencast encoder.

Deploys plugin-screencast.dll into the daemon (opcode 20 / Run), which opens
a socket on the device, reads the live front buffer via kerncore, and streams
RGB565 delta/raw frames. This host side decodes them and either benchmarks
(--frames N) or serves a live MJPEG feed (default; open the printed URL),
optionally recording to a video file at the same time (--record out.mp4).

Self-healing: waits for the nativeapp daemon to come back if it restarts,
re-triggers the plugin if it died, and reconnects on a dropped stream.

Wire (per frame): [u8 type][u32 len LE][payload]
  type 0 DELTA: runs of [u16 skip][u16 copy][copy * u16 RGB565]; skipped
                pixels keep their previous-frame value.
  type 1 RAW:   len == W*H*2 bytes of RGB565.
"""
import argparse
import io
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 272, 480
NPIX = W * H
TOOLS = Path(__file__).resolve().parent.parent / "general"
DAEMON_PORT = 1337

_latest_jpeg = [None]
_latest_frame = [None]           # latest decoded RGB565 frame bytes (for recording)
_stop = threading.Event()
_active_sock = [None]            # the live 1339 socket (shared: decode reads, input writes)
_sock_lock = threading.Lock()


def send_input(action, x, y):
    """Send one input command [u8 action][u16 x][u16 y] to the daemon over the
    live stream socket (full-duplex). action: 0=tap 1=down 2=move 3=up."""
    pkt = struct.pack("<BHH", action & 0xFF, x & 0xFFFF, y & 0xFFFF)
    with _sock_lock:
        s = _active_sock[0]
        if s is None:
            return
        try:
            s.sendall(pkt)
        except OSError:
            pass


# ── frame decode ─────────────────────────────────────────────────────────
def rgb565_to_rgb(buf):
    a = np.frombuffer(bytes(buf), dtype="<u2").reshape(H, W).astype(np.uint32)
    r = ((a >> 11) & 0x1F) << 3
    g = ((a >> 5) & 0x3F) << 2
    b = (a & 0x1F) << 3
    return np.dstack([r, g, b]).astype(np.uint8)


def recv_exact(sock, n):
    out = bytearray()
    while len(out) < n:
        c = sock.recv(n - len(out))
        if not c:
            return None
        out += c
    return bytes(out)


def apply_delta(prev, payload):
    cur = bytearray(prev)
    o = pos = 0
    plen = len(payload)
    while o + 4 <= plen:
        skip = payload[o] | (payload[o + 1] << 8)
        copy = payload[o + 2] | (payload[o + 3] << 8)
        o += 4
        pos += skip
        if copy:
            nb = copy * 2
            cur[pos * 2: pos * 2 + nb] = payload[o:o + nb]
            o += nb
            pos += copy
        if skip == 0 and copy == 0:
            break
    return cur


# ── daemon / plugin lifecycle (with recovery) ────────────────────────────
def daemon_up(ip, timeout=3.0):
    # Require the "Hello" banner: a wedged daemon still accepts the TCP
    # connection (listen backlog) but never sends the banner, so a bare
    # connect() is not proof of liveness.
    try:
        s = socket.create_connection((ip, DAEMON_PORT), timeout=timeout)
        s.settimeout(timeout)
        try:
            return s.recv(6) == b"Hello\n"
        finally:
            s.close()
    except OSError:
        return False


def wait_for_daemon(ip):
    announced = False
    while not _stop.is_set():
        if daemon_up(ip):
            if announced:
                print("nativeapp daemon back up.")
            return True
        if not announced:
            print("waiting for nativeapp daemon to come up...", file=sys.stderr)
            announced = True
        time.sleep(1.0)
    return False


def deploy(ip, local_dll, dev_dll):
    for attempt in range(12):
        if _stop.is_set():
            return False
        r = subprocess.run(
            ["python3", str(TOOLS / "lyra-write-file.py"), ip, local_dll, dev_dll,
             "--chunk-size", "1024"], capture_output=True, text=True)
        if r.returncode == 0:
            return True
        print(f"  deploy attempt {attempt + 1} failed; retrying", file=sys.stderr)
        time.sleep(1.5)
    return False


def _repl(ip, header, body=b"", timeout=15.0):
    """Send a 32-byte opcode header (+body) to nativeapp, return 32-byte resp."""
    s = socket.create_connection((ip, DAEMON_PORT), timeout=timeout)
    s.settimeout(timeout)
    try:
        if s.recv(6) != b"Hello\n":
            return None
        s.sendall(bytes(header) + body)
        resp = b""
        while len(resp) < 32:
            c = s.recv(32 - len(resp))
            if not c:
                break
            resp += c
        return resp if len(resp) == 32 else None
    finally:
        s.close()


def spawn_daemon(ip, dev_dll, port, frame_ms, entry="RunDaemon"):
    """opcode 21: load plugin, run entry on a tracked thread (REPL stays free).
    Returns daemon_id, or 0 on failure."""
    path_b = dev_dll.encode("utf-8")
    entry_b = entry.encode("ascii")
    arg = struct.pack("<HH", port, frame_ms)
    hdr = bytearray(32)
    hdr[0] = 21
    hdr[1:5] = struct.pack("<I", len(path_b))
    hdr[5:9] = struct.pack("<I", len(entry_b))
    hdr[9:13] = struct.pack("<I", len(arg))
    try:
        resp = _repl(ip, hdr, path_b + entry_b + arg)
    except OSError:
        return 0
    if not resp or resp[0] != 21:
        return 0
    return struct.unpack_from("<I", resp, 1)[0]


def stop_daemon(ip, daemon_id):
    """opcode 22: signal stop, join the daemon thread, FreeLibrary."""
    if not daemon_id:
        return
    hdr = bytearray(32)
    hdr[0] = 22
    hdr[1:5] = struct.pack("<I", daemon_id)
    try:
        _repl(ip, hdr)
    except OSError:
        pass


def connect_stream(ip, port, attempts=40):
    for _ in range(attempts):
        if _stop.is_set():
            return None
        try:
            s = socket.create_connection((ip, port), timeout=10)
            # Input commands are tiny ([action][x][y]); without NODELAY, Nagle +
            # delayed-ACK holds them 40-200ms behind the bulk frame stream.
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(15)
            return s
        except OSError:
            time.sleep(0.25)
    return None


def decode_stream(sock, on_frame, max_frames=0):
    """Decode frames until the stream drops or max_frames hit. Returns count."""
    prev = bytearray(NPIX * 2)
    n = 0
    t0 = time.time()
    nbytes = 0
    last = None
    with _sock_lock:
        _active_sock[0] = sock
    try:
        while not _stop.is_set():
            hdr = recv_exact(sock, 5)
            if hdr is None:
                break
            plen = struct.unpack_from("<I", hdr, 1)[0]
            payload = recv_exact(sock, plen)
            if payload is None:
                break
            last = bytearray(payload) if hdr[0] == 1 else apply_delta(prev, payload)
            prev = last
            n += 1
            nbytes += 5 + plen
            on_frame(last)
            if max_frames and n >= max_frames:
                break
    finally:
        with _sock_lock:
            _active_sock[0] = None
        sock.close()
    dt = time.time() - t0
    if n:
        print(f"{n} frames in {dt:.1f}s = {n/dt:.1f} fps, "
              f"{nbytes/1024:.0f} KB ({nbytes/1024/max(dt,0.001):.0f} KB/s)")
    return n, last


def supervise(ip, local_dll, dev_dll, port, frame_ms, no_deploy, on_frame, max_frames):
    """Deploy + spawn (opcode 21) + connect/decode, with recovery. The daemon
    serves repeated reconnects from one spawn, so a dropped stream just
    reconnects; only a failed connect (daemon gone, e.g. nativeapp restart)
    re-spawns. Loops until _stop (or max_frames in benchmark mode)."""
    daemon_id = 0
    deployed = no_deploy
    try:
        while not _stop.is_set():
            if not no_deploy:
                if not wait_for_daemon(ip):
                    break
                if not deployed:
                    print(f"deploying {Path(local_dll).name} -> {dev_dll}")
                    if not deploy(ip, local_dll, dev_dll):
                        print("deploy failed", file=sys.stderr)
                        break
                    deployed = True
                if not daemon_id:
                    daemon_id = spawn_daemon(ip, dev_dll, port, frame_ms)
                    if not daemon_id:
                        print("spawn failed; retrying", file=sys.stderr)
                        time.sleep(1.0)
                        continue
                    print(f"spawned screencast daemon id={daemon_id}")
                    time.sleep(1.0)  # let it bind/listen
            sock = connect_stream(ip, port)
            if sock is None:
                if _stop.is_set():
                    break
                # daemon likely gone (nativeapp restart); clear + re-spawn
                print("could not connect; re-spawning daemon", file=sys.stderr)
                if not no_deploy:
                    stop_daemon(ip, daemon_id)
                    daemon_id = 0
                time.sleep(0.5)
                continue
            print(f"streaming from {ip}:{port}")
            n, last = decode_stream(sock, on_frame, max_frames)
            if max_frames:
                return last
            print("stream dropped; reconnecting")  # daemon still alive, just reconnect
            time.sleep(0.3)
    finally:
        if not no_deploy:
            stop_daemon(ip, daemon_id)
    return None


# ── local recording (ffmpeg) ─────────────────────────────────────────────
class Recorder:
    """Pipes the live screen to a video file via ffmpeg. A pacing thread pulls the
    latest decoded RGB565 frame at a fixed cadence and writes it to ffmpeg's
    rawvideo stdin, so the file is constant-frame-rate and its duration matches
    wall-clock time: an idle screen repeats the last frame, a device-side burst
    keeps only the newest. Device frames are already rgb565le, so no conversion."""

    def __init__(self, path, fps, latest):
        self.path = path
        self.fps = max(1, fps)
        self.latest = latest         # shared [bytes|None] slot, written by decode
        self.proc = None
        self.thread = None

    def start(self):
        cmd = [
            "ffmpeg", "-loglevel", "error", "-y",
            "-f", "rawvideo", "-pix_fmt", "rgb565le", "-s", f"{W}x{H}",
            "-r", str(self.fps), "-i", "-",
            "-pix_fmt", "yuv420p", self.path,
        ]
        try:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        except FileNotFoundError:
            print("ffmpeg not found on PATH; --record needs ffmpeg installed",
                  file=sys.stderr)
            return False
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()
        print(f"recording {self.fps} fps -> {self.path}")
        return True

    def _pump(self):
        # Hold off until the first frame so the file doesn't open with blanks.
        while not _stop.is_set() and self.latest[0] is None:
            time.sleep(0.05)
        interval = 1.0 / self.fps
        next_t = time.time()
        while not _stop.is_set():
            frame = self.latest[0]
            if frame is None:
                break
            try:
                self.proc.stdin.write(frame)
            except (BrokenPipeError, OSError):
                break
            next_t += interval
            dt = next_t - time.time()
            if dt > 0:
                time.sleep(dt)
            else:
                next_t = time.time()  # fell behind; resync to now

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        print(f"saved recording -> {self.path}")


# ── MJPEG server ─────────────────────────────────────────────────────────
class MjpegHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        # JS-polling page works in every browser (Safari has no MJPEG <img>).
        if self.path == "/":
            body = (
                b"<!doctype html><html><head><meta charset=utf-8>"
                b"<title>Zune HD live</title>"
                b"<style>html,body{margin:0;height:100%;background:#111;"
                b"display:flex;align-items:center;justify-content:center}"
                b"img{height:96vh;width:auto;image-rendering:pixelated;"
                b"border:1px solid #333;cursor:crosshair;user-select:none}"
                b"</style></head><body>"
                b"<img id=s draggable=false>"
                b"<script>"
                b"const i=document.getElementById('s');"
                b"function tick(){const n=new Image();"
                b"n.onload=()=>{i.src=n.src;setTimeout(tick,33)};"
                b"n.onerror=()=>setTimeout(tick,300);"
                b"n.src='/frame.jpg?t='+Date.now();}tick();"
                b"function fb(e){const r=i.getBoundingClientRect();"
                b"let x=Math.round((e.clientX-r.left)/r.width*272);"
                b"let y=Math.round((e.clientY-r.top)/r.height*480);"
                b"return[Math.max(0,Math.min(271,x)),Math.max(0,Math.min(479,y))];}"
                b"function snd(a,p){fetch('/input?a='+a+'&x='+p[0]+'&y='+p[1]);}"
                b"let down=false,lm=0;"
                b"i.addEventListener('mousedown',e=>{e.preventDefault();down=true;snd(1,fb(e));});"
                b"window.addEventListener('mousemove',e=>{if(!down)return;"
                b"const t=Date.now();if(t-lm<30)return;lm=t;snd(2,fb(e));});"
                b"window.addEventListener('mouseup',e=>{if(!down)return;down=false;snd(3,fb(e));});"
                b"i.addEventListener('dragstart',e=>e.preventDefault());"
                b"</script></body></html>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/input"):
            from urllib.parse import urlparse, parse_qs
            q = parse_qs(urlparse(self.path).query)
            try:
                send_input(int(q["a"][0]), int(q["x"][0]), int(q["y"][0]))
            except (ValueError, KeyError, IndexError):
                pass
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/frame.jpg"):
            jpg = _latest_jpeg[0]
            if jpg is None:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(jpg)))
            self.end_headers()
            try:
                self.wfile.write(jpg)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        # /stream: MJPEG for browsers that support it
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        try:
            while not _stop.is_set():
                jpg = _latest_jpeg[0]
                if jpg is None:
                    time.sleep(0.03)
                    continue
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                 + str(len(jpg)).encode() + b"\r\n\r\n" + jpg + b"\r\n")
                time.sleep(0.02)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    ap = argparse.ArgumentParser(description="Live Zune HD screen mirror")
    ap.add_argument("ip", nargs="?", default="192.168.0.100")
    ap.add_argument("--dll", default=str(Path(__file__).resolve().parents[2] / "mods" / "screencast" / "out" / "screencast" / "plugin-screencast.dll"))
    ap.add_argument("--port", type=int, default=1339)
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--frames", type=int, default=0, help="benchmark N frames then exit")
    ap.add_argument("--fps", type=int, default=10,
                    help="device-side frame-rate cap (1..60). Higher = smoother but more "
                         "kernel-lock contention with the compositor; raise cautiously.")
    ap.add_argument("--out", default="zune-live-last.png")
    ap.add_argument("--record", metavar="FILE",
                    help="also record the live feed to a video file via ffmpeg "
                         "(e.g. zune.mp4); recorded at --fps, duration = real time")
    ap.add_argument("--quality", type=int, default=80)
    ap.add_argument("--no-deploy", action="store_true")
    args = ap.parse_args()

    frame_ms = max(16, min(5000, 1000 // max(1, args.fps)))
    # --no-deploy connects to an already-running daemon (the screencast mod in
    # Desktop mode), so no plugin DLL is needed; only the spawn path stats it.
    dev_dll = ""
    if not args.no_deploy:
        dev_dll = r"\flash2\automation\screencast-%d.dll" % (int(Path(args.dll).stat().st_mtime) & 0xffffff)

    if args.frames:
        last = supervise(args.ip, args.dll, dev_dll, args.port, frame_ms, args.no_deploy,
                         on_frame=lambda cur: None, max_frames=args.frames)
        if last is not None:
            Image.fromarray(rgb565_to_rgb(last)).save(args.out)
            print(f"saved last frame -> {args.out}")
        return

    def on_frame(cur):
        _latest_frame[0] = bytes(cur)
        buf = io.BytesIO()
        Image.fromarray(rgb565_to_rgb(cur)).save(buf, "JPEG", quality=args.quality)
        _latest_jpeg[0] = buf.getvalue()

    recorder = None
    if args.record:
        recorder = Recorder(args.record, args.fps, _latest_frame)
        if not recorder.start():
            return

    t = threading.Thread(target=supervise,
                         args=(args.ip, args.dll, dev_dll, args.port, frame_ms, args.no_deploy, on_frame, 0),
                         daemon=True)
    t.start()
    srv = ThreadingHTTPServer(("127.0.0.1", args.http_port), MjpegHandler)
    print(f"live feed: http://127.0.0.1:{args.http_port}/   (Ctrl-C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        _stop.set()
        if recorder:
            recorder.stop()


if __name__ == "__main__":
    main()
