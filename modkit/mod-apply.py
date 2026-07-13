#!/usr/bin/env python3
"""modkit CLI: host-side mod tooling for Zune HD.

zuxhook's C runtime owns all manifest lowering + capability execution on-device.
This CLI does not run capabilities; it validates, builds assets, and deploys raw
mod directories for zuxhook to apply at boot.

Usage:
  mod-apply.py validate <mod_dir> [<mod_dir>...]
  mod-apply.py build    <mod_dir> [<mod_dir>...]
  mod-apply.py package  <mod_dir> [<mod_dir>...] [--out DIR]
  mod-apply.py feed     [<mod_dir>...] [--all] [--out DIR] [--base-url URL]
  mod-apply.py apply    <mod_dir> [<mod_dir>...] [--ip IP]
  mod-apply.py restart-gemstone [--ip IP]

Architecture is documented in the wiki (https://wiki.zune.moe, in progress).
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from modkit import Mod, ApplyContext, ManifestError, deploy_mods, restart_gemstone
from modkit.validate import structural_check


def cmd_validate(args):
    """Structurally parse each mod's manifest; report shape errors."""
    import json
    ok = True
    for d in args.mod_dirs:
        path = Path(d)
        try:
            mod = Mod.from_dir(path)
            raw = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
        except (ManifestError, OSError, ValueError) as e:
            print(f"✗ {path}: {e}")
            ok = False
            continue
        errs = structural_check(mod, raw)
        if errs:
            ok = False
            print(f"✗ {mod.mod_id} ({mod.name})")
            for e in errs:
                print(f"    {e}")
        else:
            n = len(mod.actions)
            decl = [k for k in ("requires", "provides", "settings", "status",
                                "status_icons", "daemons") if raw.get(k)]
            print(f"✓ {mod.mod_id} ({mod.name}), {n} action(s); "
                  f"declarative: {', '.join(decl) or 'none'}")
    return 0 if ok else 1


def cmd_build(args):
    """Compile each mod's .xui sources to .xur, expanding @asset/@mod path tokens."""
    from modkit.build import build_mod
    rc = 0
    for d in args.mod_dirs:
        mod = Mod.from_dir(Path(d))
        print(f"== Build {mod.mod_id} ==")
        try:
            build_mod(mod)
        except Exception as e:
            print(f"    ✗ {e}")
            rc = 1
    return rc


def cmd_package(args):
    """Build each mod and write a distributable <mod_id>-<version>.zmod."""
    from modkit.package import package_mod
    rc = 0
    for d in args.mod_dirs:
        mod = Mod.from_dir(Path(d))
        print(f"== Package {mod.mod_id} {mod.version} ==")
        try:
            pkg = package_mod(mod, Path(args.out), build=not args.no_build)
            print(f"    -> {pkg.path}  {pkg.size} B  sha256={pkg.sha256}")
        except Exception as e:
            print(f"    ✗ {e}")
            rc = 1
    return rc


def cmd_feed(args):
    """Package every mod and emit a repo feed.json + mods/*.zmod under --out."""
    from modkit.build_feed import build_feed

    mod_dirs = args.mod_dirs
    if args.all:
        if mod_dirs:
            sys.exit("feed: pass mod dirs or --all, not both")
        root = Path(args.mods_root)
        lyra_dir = Path(args.lyra_dir)
        # mods/ holds only feature mods; the platform's component mods live under
        # lyra/platform/ and ship folded inside the one lyra entry (structural, no
        # exclusion list). Every dir under mods/ is a feature mod by construction.
        # An experimental mod is still published; the feed carries its flag so the
        # Browse detail page can mark it. A mod that should not be in the catalog at
        # all lives outside mods/ (a dev directory), so --all never scans it.
        feature_dirs = [d for d in sorted(root.iterdir())
                        if (d / "manifest.json").is_file()]
        if not feature_dirs:
            sys.exit(f"feed --all: no feature mods under {root}")
        mod_dirs = [str(d) for d in feature_dirs]
        if (lyra_dir / "manifest.json").is_file():
            mod_dirs.append(str(lyra_dir))
            platform_dir = lyra_dir / "platform"
            n_components = sum(1 for p in sorted(platform_dir.iterdir())
                               if (p / "manifest.json").is_file()) \
                if platform_dir.is_dir() else 0
            print(f"feed --all: {len(feature_dirs)} feature mods + lyra platform "
                  f"({n_components} component mods folded in)")
        else:
            print(f"feed --all: {len(feature_dirs)} feature mods")
    elif not mod_dirs:
        sys.exit("feed: pass one or more mod dirs, or --all")

    build_feed(mod_dirs, Path(args.out), args.base_url, build=not args.no_build)
    return 0


def cmd_apply(args):
    """Deploy raw mod dirs; zuxhook lowers + applies (and owns enabled.json) at next boot."""
    mods = [Mod.from_dir(Path(d)) for d in args.mod_dirs]
    ctx = ApplyContext(device_ip=args.ip)
    deploy_mods(mods, ctx)
    print(f"\nDeployed {len(mods)} mod dir(s) to {args.ip}. "
          f"Reboot to let zuxhook apply.")
    return 0


def cmd_restart_gemstone(args):
    """Kill gemstone (compositor relaunches it) to re-run zuxhook Phase 2."""
    ctx = ApplyContext(device_ip=args.ip)
    new_pid = restart_gemstone(ctx)
    print(f"\nGemstone restarted (new pid 0x{new_pid:08x}).")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("validate", help="structural manifest check")
    p.add_argument("mod_dirs", nargs="+")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("build",
                       help="compile .xui sources to .xur, expanding "
                            "@asset/ and @mod/ device-path tokens")
    p.add_argument("mod_dirs", nargs="+")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("package",
                       help="build + zip each mod into a distributable .zmod")
    p.add_argument("mod_dirs", nargs="+")
    p.add_argument("--out", default="dist/repo/mods",
                   help="output dir for the .zmod files")
    p.add_argument("--no-build", action="store_true",
                   help="skip the .xui->.xur build step (use existing .xur)")
    p.set_defaults(func=cmd_package)

    p = sub.add_parser("feed",
                       help="package mods + emit repo feed.json under --out")
    p.add_argument("mod_dirs", nargs="*",
                   help="mod dirs to publish (or use --all)")
    p.add_argument("--all", action="store_true",
                   help="publish every feature mod under --mods-root "
                        "(system mods excluded); the repo.zune.moe catalog")
    p.add_argument("--mods-root", default="mods",
                   help="root scanned by --all (default: mods)")
    p.add_argument("--lyra-dir", default="lyra",
                   help="the Lyra platform bundle dir, packaged by --all if present "
                        "(default: lyra)")
    p.add_argument("--out", default="dist/repo",
                   help="output dir (feed.json + mods/*.zmod)")
    p.add_argument("--base-url", default="https://repo.zune.moe",
                   help="base URL the feed's package URLs are rooted at")
    p.add_argument("--no-build", action="store_true",
                   help="skip the .xui->.xur build step (use existing .xur)")
    p.set_defaults(func=cmd_feed)

    p = sub.add_parser("apply",
                       help="deploy raw mod dirs (zuxhook owns enabled.json + applies "
                            "at next boot)")
    p.add_argument("mod_dirs", nargs="+")
    p.add_argument("--ip", default="192.168.0.100")
    p.set_defaults(func=cmd_apply)

    p = sub.add_parser("restart-gemstone",
                       help="kill gemstone (compositor relaunches; "
                            "re-runs zuxhook Phase 2) by planting TerminateProcess "
                            "shellcode via kernel R/W")
    p.add_argument("--ip", default="192.168.0.100")
    p.set_defaults(func=cmd_restart_gemstone)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
