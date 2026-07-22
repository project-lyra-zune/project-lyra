#!/usr/bin/env python3
"""Assemble Project Lyra release packages in both deploy forms.

Given the three prebuilt device binaries (exploiter.exe, nativeapp.exe, zuxhook.dll)
and the mods to bundle, this builds one shared payload tree and emits:

  <out>/lyra-hd-deploykit/    a zune-deploy "Deploy Kit" folder (application.cfg + payload/)
  <out>/lyra-hd-deploykit.zip the Deploy Kit folder zipped for release download
  <out>/lyra-hd.ccgame        an XNA game package (CAB with XCabInfo.resources + members)

Both carry identical content; they differ only in container format so either deploy
path can install the same release. The game GUID and the Content\\ layout are fixed:
the exploiter launches \\gametitle\\584E07D1\\Content\\nativeapp.exe by hard-coded path,
and nativeapp mirrors Content\\ into \\flash2\\automation at launch.

Requires: python3 and a C# compiler (csc on Windows, mcs on unix); cabextract is
optional (CAB verification). Scene recompilation needs .NET 8 (XUIHelper); pass
--skip-mod-build to package the committed .xur instead (e.g. on Windows 7).
"""
import argparse, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LYRA_ROOT = HERE.parent.parent
GAME_GUID = "584e07d1-0000-0000-0000-000000000000"
GAME_NAME = "Install Project Lyra"
GAME_DESC = "Installs Lyra, a persistent mod framework and loader on the Zune HD."
RUNTIME_PROFILE = "Zune.v4.0.Beta"

_WINDOWS = os.name == "nt"


def _csharp_compile(src, out_exe):
    """Compile a C# source to an exe: csc (.NET Framework 3.5) on Windows, mcs
    (Mono) elsewhere."""
    if _WINDOWS:
        csc = shutil.which("csc") or r"C:\Windows\Microsoft.NET\Framework\v3.5\csc.exe"
        subprocess.run([csc, "/nologo", f"/out:{out_exe}", str(src)], check=True)
    else:
        subprocess.run(["mcs", f"-out:{out_exe}", str(src)], check=True)


def _run_exe(exe, args):
    """Run a .NET exe directly on Windows, under Mono elsewhere."""
    cmd = [str(exe), *(str(a) for a in args)]
    subprocess.run(cmd if _WINDOWS else ["mono", *cmd], check=True)


# Canonical source-tree Release paths for the platform binaries. The class-blob builders
# read zuxhook.dll from here (validating every vtable extern_module fixup against its PE
# export table), and `mod-apply.py feed` reads both from here when packaging the platform
# .zmod. Staging both keeps the deploykit and the published feed from ever diverging.
_FEED_ZUXHOOK   = LYRA_ROOT / "src" / "zuxhook"   / "bin" / "OpenZDK (ARMV4I)" / "Release" / "zuxhook.dll"
_FEED_NATIVEAPP = LYRA_ROOT / "src" / "nativeapp" / "bin" / "OpenZDK (ARMV4I)" / "Release" / "nativeapp.exe"
_CLASSBLOB_BUILDERS = ("build_gemmod_manager", "build_quicksettings", "build_contextlist")
_INSTALLER_DIR = LYRA_ROOT / "src" / "nativeapp" / "content" / "installer"


def _stage_binary(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if Path(src).resolve() != dst.resolve():
        shutil.copy2(src, dst)


def stage_platform_binaries(zuxhook, nativeapp):
    """Stage the prebuilt platform binaries into their source-tree Release paths, the one
    location both the class-blob builders and `mod-apply.py feed` read them from."""
    _stage_binary(zuxhook, _FEED_ZUXHOOK)
    _stage_binary(nativeapp, _FEED_NATIVEAPP)


def regenerate_source_artifacts():
    """Regenerate the committed-free build artifacts from source: gemstone class blobs
    (Python, validated against the staged zuxhook.dll) and installer splash .bgra
    (bake-installer-assets.py). Scene .xur are regenerated later by build_mods.
    Nothing here is committed; see .gitignore + BUILDING.md."""
    for mod in _CLASSBLOB_BUILDERS:
        subprocess.run([sys.executable, "-m", f"modkit.{mod}"],
                       cwd=LYRA_ROOT / "modkit", check=True)
    subprocess.run([sys.executable, "bake-installer-assets.py"],
                   cwd=_INSTALLER_DIR, check=True)


def _verify_cab(pkg):
    """Best-effort CAB integrity check; skipped if cabextract is not installed."""
    tool = shutil.which("cabextract")
    if tool:
        subprocess.run([tool, "-q", "-t", str(pkg)], check=True)


def build_mods(mods, payload_content, modkit, build_scenes=True, dst_name="mods",
               src_root=None):
    """Stage each mod under its mod_id below Content/<dst_name>/, (optionally) compile
    its .xui scenes to .xur, then prune to the deployable set (manifest + referenced
    blobs/assets) so authoring sources never reach the device. The installer mirrors
    this staged tree verbatim (mods/ = feature mods, platform/ = platform mods). Scene
    compilation (XUIHelper, .NET 8) is skippable so Windows 7 can package from the
    committed .xur; the .xur are device-path stable. src_root is the source tree the
    mod names resolve against (default mods/; lyra/platform/ for platform components)."""
    if not mods:
        return
    sys.path.insert(0, str(LYRA_ROOT / "modkit"))
    from modkit.manifest import Mod
    from modkit.payload import deployable_files

    mods_root = Path(src_root) if src_root else LYRA_ROOT / "mods"
    dst_mods = payload_content / dst_name
    dst_mods.mkdir(parents=True, exist_ok=True)
    for name in mods:
        manifest = mods_root / name / "manifest.json"
        if not manifest.is_file():
            sys.exit(f"mod not found: {manifest}")
        mod_id = json.loads(manifest.read_text())["mod_id"]
        staged = dst_mods / mod_id
        shutil.copytree(mods_root / name, staged)
        if build_scenes:
            subprocess.run([sys.executable, str(modkit), "build", str(staged)], check=True)

        keep = deployable_files(Mod.from_dir(staged))
        for f in sorted(staged.rglob("*"), reverse=True):
            if f.is_file() and f.resolve() not in keep:
                f.unlink()
            elif f.is_dir() and not any(f.iterdir()):
                f.rmdir()


INSTALLER_ASSETS = LYRA_ROOT / "src" / "nativeapp" / "content" / "installer"


def stage_installer_ui(content, args):
    """Stage the splash assets into Content: the committed .bgra textures and the
    compiled wash shaders (built next to nativeapp.exe). Loaded from Content;
    install() does not mirror them into \\flash2\\automation."""
    blobs = sorted(INSTALLER_ASSETS.glob("*.bgra"))
    if not blobs:
        sys.exit(f"installer assets missing; run {INSTALLER_ASSETS}/bake-installer-assets.py")
    for b in blobs:
        shutil.copy2(b, content / b.name)

    shader_dir = Path(args.shaders) if args.shaders else Path(args.nativeapp).parent
    for name in ("wash.nvbv", "wash.nvbf"):
        src = shader_dir / name
        if not src.is_file():
            sys.exit(f"compiled shader missing: {src} (built next to nativeapp.exe)")
        shutil.copy2(src, content / name)


def assemble_payload(payload, args):
    """payload/exploiter.exe + payload/Content/{nativeapp.exe, zuxhook.dll, lyra.json,
    platform/<component>/..., mods/<feature>/...}. The installer mirrors Content into
    \\flash2\\automation: platform mods -> platform\\, feature mods -> mods\\."""
    content = payload / "Content"
    content.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.exploiter, payload / "exploiter.exe")
    shutil.copy2(args.nativeapp, content / "nativeapp.exe")
    shutil.copy2(args.zuxhook, content / "zuxhook.dll")

    # lyra/manifest.json ships verbatim as lyra.json (the same file the .zmod carries and
    # the scanner reads) so both delivery paths are one source. The platform's component
    # mods are the dirs under lyra/platform/ (structural, no manifest list); they stage
    # into Content/platform/ and the installer mirrors that to \flash2\automation\platform\.
    lyra_platform = LYRA_ROOT / "lyra" / "platform"
    shutil.copy2(LYRA_ROOT / "lyra" / "manifest.json", content / "lyra.json")
    components = [p.name for p in sorted(lyra_platform.iterdir())
                  if (p / "manifest.json").is_file()]
    build_mods(components, content, LYRA_ROOT / "modkit" / "mod-apply.py",
               build_scenes=not args.skip_mod_build, dst_name="platform",
               src_root=lyra_platform)

    build_mods(args.mods, content, LYRA_ROOT / "modkit" / "mod-apply.py",
               build_scenes=not args.skip_mod_build, dst_name="mods")
    stage_installer_ui(content, args)


def emit_deploykit(payload, thumbnail, out):
    kit = out / "lyra-hd-deploykit"
    if kit.exists():
        shutil.rmtree(kit)
    kit.mkdir(parents=True)
    shutil.copytree(payload, kit / "payload")
    shutil.copy2(thumbnail, kit / "GameThumbnail.png")
    (kit / "application.cfg").write_text(
        f"guid: {GAME_GUID}\n"
        f"name: {GAME_NAME}\n"
        f"description: {GAME_DESC}\n"
        f"exec: exploiter.exe\n"
        f"src: payload\n"
        f"thumbnail: GameThumbnail.png\n"
        f"compatibility: hd\n"
    )
    return kit


def emit_ccgame(payload, thumbnail, out, work):
    # Container paths are relative to the payload root, backslash-separated. The CAB
    # member named "N" maps to Files[N] in XCabInfo, so order must stay consistent.
    files = sorted(
        (str(p.relative_to(payload)).replace("/", "\\"), p)
        for p in payload.rglob("*") if p.is_file()
    )
    if not any(c == "exploiter.exe" for c, _ in files):
        sys.exit("exploiter.exe missing from payload")

    rows = ",\n                ".join('{ "%s" }' % c.replace("\\", "\\\\") for c, _ in files)
    cs = work / "CreateXCabInfo.cs"
    cs.write_text('''using System; using System.IO; using System.Resources;
public static class CreateXCabInfo {
    public static void Main(string[] args) {
        using (var w = new ResourceWriter(args[1])) {
            w.AddResource("GameTitle", "%s");
            w.AddResource("GameDescription", "%s");
            w.AddResource("StartupAssembly", "exploiter.exe");
            w.AddResource("RuntimeProfile", "%s");
            w.AddResource("GameGuid", new Guid("%s"));
            w.AddResource("Files", new string[,] {
                %s
            });
            w.AddResource("GameThumbnail", File.ReadAllBytes(args[0]));
        }
    }
}''' % (GAME_NAME, GAME_DESC, RUNTIME_PROFILE, GAME_GUID, rows))
    exe = work / "CreateXCabInfo.exe"
    res = work / "XCabInfo.resources"
    _csharp_compile(cs, exe)
    _run_exe(exe, [thumbnail, res])

    pkg = out / "lyra-hd.ccgame"
    members = [f"XCabInfo.resources={res}"] + [f"{i}={disk}" for i, (_, disk) in enumerate(files)]
    subprocess.run([sys.executable, str(HERE / "make_store_cab.py"), "--out", str(pkg)] + members, check=True)
    _verify_cab(pkg)
    return pkg


def main():
    ap = argparse.ArgumentParser(description="Build Project Lyra release packages (deploykit + ccgame).")
    ap.add_argument("--exploiter", required=True, help="prebuilt exploiter.exe")
    ap.add_argument("--nativeapp", required=True, help="prebuilt nativeapp.exe")
    ap.add_argument("--zuxhook", required=True, help="prebuilt zuxhook.dll")
    ap.add_argument("--shaders", default=None,
                    help="dir holding compiled wash.nvbv/wash.nvbf (default: nativeapp.exe's dir)")
    ap.add_argument("--thumbnail", default=str(HERE / "GameThumbnail.png"))
    ap.add_argument("--mods", nargs="*", default=[],
                    help="feature mods to pre-bundle in Content/mods/ (default: none; "
                         "the platform's component mods come from lyra/platform/)")
    ap.add_argument("--skip-mod-build", action="store_true",
                    help="reuse on-disk .xur instead of recompiling scenes from .xui "
                         "(XUIHelper needs .NET 8; only for local iteration)")
    ap.add_argument("--skip-artifact-regen", action="store_true",
                    help="reuse on-disk class blobs + .bgra instead of regenerating "
                         "them from source (only for local iteration)")
    ap.add_argument("--out", default=str(LYRA_ROOT / "dist"))
    args = ap.parse_args()

    stage_platform_binaries(args.zuxhook, args.nativeapp)
    if not args.skip_artifact_regen:
        regenerate_source_artifacts()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lyra-pkg-") as tmp:
        work = Path(tmp)
        payload = work / "payload"
        assemble_payload(payload, args)
        kit = emit_deploykit(payload, Path(args.thumbnail), out)
        pkg = emit_ccgame(payload, Path(args.thumbnail), out, work)

    kit_zip = shutil.make_archive(str(kit), "zip", root_dir=str(out), base_dir=kit.name)
    n = sum(1 for _ in (kit / "payload").rglob("*") if _.is_file())
    print(f"deploykit: {kit}  ({n} payload files)")
    print(f"deploykit zip: {kit_zip}")
    print(f"ccgame:    {pkg}")


if __name__ == "__main__":
    main()
