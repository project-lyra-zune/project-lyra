#!/usr/bin/env python3
"""Capture the Zune HD's live screen to a PNG.

Deterministic, no scanning: read a fixed kernel address that holds the
current display front-buffer physical address, pull only that one
framebuffer from the carveout, and decode it.

Display framebuffer (device-validated 2026-05-28):
  format  A8R8G8B8  (memory byte order B,G,R,A; alpha ignored)
  size    272 x 480 portrait, pitch 1088 bytes (tight), frame 0x7F800
  carveout: PA 0x06000000 mapped at kernel VA 0xd0970000 (32 MB)

Front-buffer PA source:
  EMPIRICAL (current default): *(uint32*)0xc07effe8 in libnvddk_disp .data
    holds the live front-buffer PA and tracks buffer flips. Validated live.
  CANONICAL (preferred once pinned): the DC START_ADDR register, at
    DC_VA = *(uint32*)0xc07ef0e0, value = (reg & 0xFFFFFF) << 10. The
    register byte offset must be pinned on-device once (see
    notes/re-2026-05-28-screencap/libnvddk_disp-framebuffer-RE.md). When
    known, set --dc-start-off to use it (ground-truth displayed buffer).

See notes/re-2026-05-28-screencap/ for the full RE + validation record.
"""
import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

FB_PTR_FIELD = 0xC07EFFE8       # libnvddk_disp .data: live front-buffer PA
DC_MAP_STRUCT = 0xC07EF0E0      # [+0]=DC reg VA, [+4]=DC PA, [+8]=size
CARVEOUT_PA_BASE = 0x06000000
CARVEOUT_KVA = 0xD0970000
CARVEOUT_SIZE = 0x02000000
W, H, STRIDE = 272, 480, 1088
FRAME = STRIDE * H              # 0x7F800
FLASH = r"\flash2\automation\screencap.raw"
SANITIZED = "flash2__automation__screencap.raw"


def retry(fn, what, tries=8, delay=2.0):
    last = None
    for i in range(tries):
        try:
            return fn()
        except Exception as e:  # noqa: BLE001 (flaky link, retry anything)
            last = e
            print(f"  {what}: attempt {i+1} {type(e).__name__}; retrying", file=sys.stderr)
            time.sleep(delay)
    raise SystemExit(f"{what}: gave up after {tries} tries ({last})")


def front_buffer_pa(r, dc_start_off):
    if dc_start_off is not None:
        dc_va = struct.unpack("<I", retry(lambda: r.kread(DC_MAP_STRUCT, 4), "read DC_VA"))[0]
        reg = struct.unpack("<I", retry(lambda: r.kread(dc_va + dc_start_off, 4), "read START_ADDR"))[0]
        return (reg & 0x00FFFFFF) << 10
    return struct.unpack("<I", retry(lambda: r.kread(FB_PTR_FIELD, 4), "read FB ptr"))[0]


def capture(ip, out, dc_start_off=None):
    r = ZuneREPL(ip, timeout=30.0)
    pa = front_buffer_pa(r, dc_start_off)
    if not (CARVEOUT_PA_BASE <= pa < CARVEOUT_PA_BASE + CARVEOUT_SIZE):
        raise SystemExit(f"front-buffer PA {pa:#x} not in carveout (source field stale?)")
    kva = pa - CARVEOUT_PA_BASE + CARVEOUT_KVA
    print(f"front buffer: PA {pa:#x}  kva {kva:#x}")

    st, tot = retry(lambda: r.dump_va_to_file(kva, FRAME, FLASH), "dump framebuffer")
    if st != 0 or tot != FRAME:
        raise SystemExit(f"dump failed st={st} tot={tot}")

    tmp = Path(out).parent / "_screencap_tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    part = tmp / SANITIZED

    def pull():
        if part.exists():
            part.unlink()
        subprocess.run(
            ["python3", str(Path(__file__).resolve().parent.parent / "general" / "lyra-read-file.py"), ip, FLASH,
             "--output-dir", str(tmp), "--max-bytes", str(FRAME + 4096), "--timeout", "30"],
            check=True, capture_output=True)
        if not part.exists() or part.stat().st_size != FRAME:
            raise RuntimeError("pull incomplete")
        return part.read_bytes()

    buf = np.frombuffer(retry(pull, "pull framebuffer"), dtype=np.uint8)
    px = buf[:FRAME].reshape(H, STRIDE)[:, : W * 4].reshape(H, W, 4)
    rgb = np.dstack([px[:, :, 2], px[:, :, 1], px[:, :, 0]]).astype(np.uint8)  # BGRA -> RGB
    Image.fromarray(rgb).save(out)
    print(f"saved {out}")


def main():
    ap = argparse.ArgumentParser(description="Capture the Zune HD screen to PNG")
    ap.add_argument("ip", nargs="?", default="192.168.0.100")
    ap.add_argument("--out", default="zune-screen.png")
    ap.add_argument("--dc-start-off", type=lambda s: int(s, 0), default=None,
                    help="DC START_ADDR register byte offset (use the canonical "
                         "ground-truth source instead of the .data shortcut)")
    args = ap.parse_args()
    capture(args.ip, args.out, args.dc_start_off)


if __name__ == "__main__":
    main()
