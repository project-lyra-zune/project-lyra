#!/usr/bin/env python3
"""Build the install.zune.moe page. Called by publish-repo.sh."""

import argparse
import base64
import json
import pathlib
import shutil
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
LYRA = HERE.parent


def stub_bytes(stub_json: pathlib.Path) -> list:
    if not stub_json.exists():
        subprocess.run([sys.executable, str(HERE / "stub" / "build-stub.py")], check=True)
    data = json.loads(stub_json.read_text())
    blob = data["bytes"]
    if len(blob) % 2:
        sys.exit("stub is an odd number of bytes; it must pair evenly into UCS-2 units")
    return blob


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="platform version, e.g. 1.3.0")
    ap.add_argument("--image", type=pathlib.Path,
                    default=LYRA / "src" / "lyraboot" / "bin" / "lyraboot.exe")
    ap.add_argument("--stub", type=pathlib.Path, default=HERE / "stub" / "stub.json")
    ap.add_argument("--chain", type=pathlib.Path, default=HERE / "chain.js")
    ap.add_argument("--template", type=pathlib.Path, default=HERE / "index.html.in")
    ap.add_argument("--about", type=pathlib.Path, default=HERE / "about.html.in")
    ap.add_argument("--pin-url", help="bundle URL the bootstrap must already carry")
    ap.add_argument("--pin-sha", help="bundle sha256 the bootstrap must already carry")
    ap.add_argument("-o", "--out", type=pathlib.Path, required=True,
                    help="output directory, e.g. repo/install")
    args = ap.parse_args()

    if not args.image.exists():
        sys.exit(f"{args.image} not built; run tools/win7-build.sh lyraboot")

    image = args.image.read_bytes()

    # The binary is built on a separate machine, so verify it carries the pin being
    # published rather than trusting that boot_pin.h and the build agree.
    for label, value in (("version", args.version), ("url", args.pin_url), ("sha256", args.pin_sha)):
        if value and value.encode() not in image:
            sys.exit(f"{args.image.name} does not carry {label} {value!r}.\n"
                     f"boot_pin.h changed since it was built: tools/win7-build.sh lyraboot")
    stub = stub_bytes(args.stub)

    # The stub reads a little-endian count then that many bytes. Padded to an even
    # length to pair into UCS-2 units; the pad sits outside the count.
    payload = struct.pack("<I", len(image)) + image
    if len(payload) % 2:
        payload += b"\0"

    b64_name = f"lyraboot-{args.version}.b64"
    page = args.template.read_text()
    for token, value in (
        ("__CHAIN__", args.chain.read_text().rstrip("\n")),
        ("__STUB_BYTES__", "[" + ",".join(str(b) for b in stub) + "]"),
        ("__IMAGE_URL__", b64_name),
        ("__IMAGE_BYTES__", str(len(payload))),
        ("__VERSION__", args.version),
    ):
        if token not in page:
            sys.exit(f"template has no {token} to stamp")
        page = page.replace(token, value)

    # Served to anything arriving over HTTPS, which the device never does.
    about = args.about.read_text().replace("__VERSION__", args.version)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "index.html").write_text(page)
    (args.out / "about.html").write_text(about)
    shutil.copyfile(HERE / "lyra.png", args.out / "lyra.png")
    b64 = base64.b64encode(payload).decode()
    (args.out / b64_name).write_text("\n".join(b64[i:i + 76] for i in range(0, len(b64), 76)) + "\n")

    print(f"page:  {args.out / 'index.html'}  ({len(page):,} bytes)")
    print(f"image: {args.out / b64_name}  ({len(b64):,} b64 chars for {len(payload):,} bytes)")
    print(f"stub:  {len(stub)} bytes, shellcode string {(len(stub) + len(payload)) // 2:,} units")


if __name__ == "__main__":
    main()
