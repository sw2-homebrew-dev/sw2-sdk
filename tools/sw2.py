#!/usr/bin/env python3
"""sw2 - one front door for the Switch 2 homebrew SDK.

  sw2 doctor                         check the toolchain
  sw2 new <dir> "Name" [--gpubrew] [--description TEXT]
  sw2 build <app-dir> [...]          native (manifest.json) or GPUBrew (app.json)
  sw2 package <app-dir> [--out DIR] [--url-base URL] [--author A] [--category C]
                                     deterministic zip + sha256 + catalog entry
  sw2 host-bundle [--out FILE] [--version V]
                                     prebuilt SDK tarball (+ .sha256): what releases
                                     ship and what a host without a toolchain runs
  sw2 run [hbl-start args...]        start the launcher (e.g. --mock)
  sw2 store <list|install|remove|update|refresh> [id...]
                                     desktop store (the on-console store's fallback)
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = ROOT / "sdk"
GPUBREW = SDK / "gpubrew"
ID_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
# Fixed timestamp keeps zips byte-identical between builds.
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


def fail(message: str) -> int:
    print(f"sw2: {message}", file=sys.stderr)
    return 1


def which(*names):
    for name in names:
        env = os.environ.get(name.upper().replace("-", "_"))
        if env:
            return env
        found = shutil.which(name)
        if found:
            return found
    return None


def app_kind(directory: pathlib.Path) -> str:
    if (directory / "manifest.json").exists():
        return "native"
    if (directory / "app.json").exists():
        return "gpubrew"
    raise SystemExit(fail(f"{directory}: no manifest.json or app.json"))


def read_manifest(directory: pathlib.Path) -> dict:
    name = "manifest.json" if app_kind(directory) == "native" else "app.json"
    return json.loads((directory / name).read_text())


# --- doctor ---------------------------------------------------------------

def cmd_doctor(_args) -> int:
    sys.path.insert(0, str(SDK / "tools"))
    import toolchain
    chosen = toolchain.select()
    mac = sys.platform == "darwin"
    checks = [
        ("node (host runtime, store)", which("node")),
        ("AArch64 toolchain (native apps)",
         f"{chosen['kind']}: {chosen['cc'][0]}" if chosen else None),
        ("glslangValidator (shaders)", which("glslangValidator")),
        ("spirv-val (shader validation)", which("spirv-val")),
    ]
    if not mac:
        checks.append(("qemu-aarch64 (offline runtime tests)", which("qemu-aarch64")))
    missing = 0
    for label, path in checks:
        print(f"  {'ok ' if path else 'MISSING'}  {label}{'  ' + path if path else ''}")
        missing += not path
    try:
        import PIL  # noqa: F401
        print("  ok       python Pillow (icons, font atlas)")
    except ImportError:
        print("  MISSING  python Pillow (icons, font atlas)")
        missing += 1
    if missing and mac:
        print("\nmacOS (Homebrew): brew install node python llvm lld glslang spirv-tools && "
              "python3 -m pip install --user Pillow\n"
              "optional: brew install --cask font-dejavu (only to regenerate the font atlas)")
    elif missing:
        print("\nDebian/Ubuntu/Mint: sudo apt install nodejs python3-pil gcc-aarch64-linux-gnu "
              "binutils-aarch64-linux-gnu glslang-tools spirv-tools qemu-user "
              "mesa-vulkan-drivers fonts-dejavu-core\n"
              "(clang lld llvm also work in place of the gcc-aarch64 packages)")
    return 0 if not missing else 1


# --- new ------------------------------------------------------------------

def cmd_new(args) -> int:
    target = pathlib.Path(args.dir).resolve()
    if not args.gpubrew:
        command = [sys.executable, str(SDK / "tools/hbx-new.py"), str(target), args.name]
        if args.description:
            command.append(args.description)
        return subprocess.call(command)
    if not ID_PATTERN.fullmatch(target.name):
        return fail("app id (folder name) must be lowercase [a-z0-9._-]")
    if target.exists() and any(target.iterdir()):
        return fail(f"{target} already exists and is not empty")
    template = GPUBREW / "template"
    for source in template.rglob("*"):
        if source.is_dir():
            continue
        destination = target / source.relative_to(template)
        destination.parent.mkdir(parents=True, exist_ok=True)
        text = source.read_text()
        text = (text.replace("{{ID}}", target.name).replace("{{NAME}}", args.name)
                    .replace("{{DESCRIPTION}}", args.description or "A GPUBrew shader app"))
        destination.write_text(text)
    (target / "assets").mkdir(exist_ok=True)
    print(f"created {target}\nnext: sw2 build {target}")
    return 0


# --- build ----------------------------------------------------------------

def build_gpubrew(directory: pathlib.Path) -> int:
    glslang = which("glslangValidator")
    if not glslang:
        return fail("glslangValidator not found (sw2 doctor)")
    spirv_val = which("spirv-val")
    manifest = json.loads((directory / "app.json").read_text())
    shaders = directory / "shaders"
    for stage, key in (("vert", "vertex"), ("frag", "fragment"), ("comp", "compute")):
        source = shaders / f"app.{stage}"
        if not source.exists():
            continue
        output = directory / manifest["shaders"][key]
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([glslang, "-V", "-Os", "-g0", "--target-env", "vulkan1.0",
                        "-I" + str(GPUBREW / "include"), "-I" + str(shaders),
                        str(source), "-o", str(output)], check=True)
        if spirv_val:
            subprocess.run([spirv_val, "--target-env", "vulkan1.0", str(output)], check=True)
    state_name = manifest.get("initialState")
    if state_name and not (directory / state_name).exists():
        # Default state: start faded out (transition.x = 1.0 at byte 960).
        state = bytearray(1024)
        struct.pack_into("<f", state, 960, 1.0)
        (directory / state_name).parent.mkdir(parents=True, exist_ok=True)
        (directory / state_name).write_bytes(bytes(state))
    print(f"built {manifest['id']} (gpubrew)")
    return 0


def cmd_build(args) -> int:
    for item in args.dirs:
        directory = pathlib.Path(item).resolve()
        if app_kind(directory) == "native":
            status = subprocess.call([sys.executable, str(SDK / "tools/hbx-build.py"), str(directory)])
        else:
            status = build_gpubrew(directory)
        if status:
            return status
    return 0


# --- package --------------------------------------------------------------

def package_files(directory: pathlib.Path, manifest: dict, kind: str):
    names = set()
    if kind == "native":
        names.add("manifest.json")
        for name in ("code.bin", "data.bin", "report.json"):
            if (directory / "build" / name).exists():
                names.add(f"build/{name}")
        if "build/report.json" not in names:
            raise SystemExit(fail("not built: run sw2 build first"))
        assets = directory / "assets"
        if assets.exists():
            names.update(str(p.relative_to(directory)) for p in assets.rglob("*") if p.is_file())
    else:
        names.add("app.json")
        refs = list(manifest.get("shaders", {}).values())
        refs += [manifest.get(k) for k in ("initialState", "data")]
        seq = manifest.get("computeSequence") or {}
        refs += [seq.get("begin"), seq.get("after")]
        refs.append((manifest.get("memory") or {}).get("initial"))
        refs += list(((manifest.get("audio") or {}).get("effects") or {}).values())
        for ref in filter(None, refs):
            if not (directory / ref).exists():
                raise SystemExit(fail(f"{ref} missing (run sw2 build)"))
            names.add(ref)
    icon = manifest.get("icon") or "icon.png"
    if (directory / icon).exists():
        names.add(icon)
    return sorted(names)


def cmd_package(args) -> int:
    directory = pathlib.Path(args.dir).resolve()
    kind = app_kind(directory)
    manifest = read_manifest(directory)
    app_id, version = manifest["id"], str(manifest["version"])
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"{app_id}-{version}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name in package_files(directory, manifest, kind):
            info = zipfile.ZipInfo(f"{app_id}/{name}", ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, (directory / name).read_bytes())
    data = archive.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    (out / f"{archive.name}.sha256").write_text(f"{digest}  {archive.name}\n")
    url_base = (args.url_base or "").rstrip("/")
    entry = {
        "id": app_id,
        "name": manifest["name"],
        "kind": kind,
        "version": version,
        "description": manifest.get("description", ""),
        "author": args.author or "",
        "category": args.category or "app",
        "license": args.license or "",
        "source_repo": args.source_repo or "",
        "artifact_url": f"{url_base}/{archive.name}" if url_base else archive.name,
        "sha256": digest,
        "size": len(data),
        "icon_url": args.icon_url or "",
    }
    (out / f"{app_id}.catalog.json").write_text(json.dumps(entry, indent=2) + "\n")
    print(f"{archive}\nsha256 {digest}\ncatalog entry: {out / (app_id + '.catalog.json')}")
    return 0


# --- host bundle ----------------------------------------------------------

BUNDLE_SKIP_DIRS = {".git", "__pycache__", "node_modules", "saves", "run", "release", "out"}


def cmd_host_bundle(args) -> int:
    import tarfile
    required = [ROOT / "launcher/build/code.bin", ROOT / "sdk/runners/gpubrew/build/code.bin"]
    missing = [str(p.relative_to(ROOT)) for p in required if not p.exists()]
    if missing:
        return fail("build first (sh tools/build-all.sh); missing " + ", ".join(missing))
    commit = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"],
                            capture_output=True, text=True).stdout.strip() or "unknown"
    version = args.version or subprocess.run(
        ["git", "-C", str(ROOT), "describe", "--tags", "--always", "--dirty"],
        capture_output=True, text=True).stdout.strip() or commit
    out = pathlib.Path(args.out or f"sw2-sdk-host-{commit}.tar.gz").resolve()

    def keep(info):
        parts = pathlib.PurePosixPath(info.name).parts
        if any(part in BUNDLE_SKIP_DIRS for part in parts):
            return None
        if info.name.endswith((".log", ".o")) or "/build/tests" in info.name:
            return None
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    with tarfile.open(out, "w:gz") as tar:
        tar.add(ROOT, arcname="sw2-sdk", filter=keep)
        stamp = json.dumps({"version": version, "commit": commit}, indent=2).encode()
        info = tarfile.TarInfo("sw2-sdk/BUNDLE.json")
        info.size = len(stamp)
        import io
        tar.addfile(info, io.BytesIO(stamp))
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    pathlib.Path(str(out) + ".sha256").write_text(f"{digest}  {out.name}\n")
    print(f"{out}\nsha256 {digest}\nunpack and run: tar xzf {out.name} && sw2-sdk/sw2 run --run <held-run-folder>")
    return 0


# --- run / store ----------------------------------------------------------

def node() -> str:
    found = os.environ.get("HBL_NODE") or which("node")
    if not found:
        raise SystemExit(fail("node not found"))
    return found


def cmd_run(args) -> int:
    return subprocess.call([str(ROOT / "hbl-start"), *args.rest])


def cmd_store(args) -> int:
    return subprocess.call([node(), str(ROOT / "host/store-cli.cjs"), *args.rest])


def main() -> int:
    # run/store forward everything verbatim (argparse would swallow "--mock").
    if len(sys.argv) > 1 and sys.argv[1] in ("run", "store"):
        rest = argparse.Namespace(rest=sys.argv[2:])
        return (cmd_run if sys.argv[1] == "run" else cmd_store)(rest)
    parser = argparse.ArgumentParser(prog="sw2", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor").set_defaults(fn=cmd_doctor)
    p = sub.add_parser("host-bundle")
    p.add_argument("--out")
    p.add_argument("--version", help="version recorded in BUNDLE.json (default: git describe)")
    p.set_defaults(fn=cmd_host_bundle)
    p = sub.add_parser("new")
    p.add_argument("dir"); p.add_argument("name")
    p.add_argument("--gpubrew", action="store_true", help="shader app instead of native C")
    p.add_argument("--description")
    p.set_defaults(fn=cmd_new)
    p = sub.add_parser("build")
    p.add_argument("dirs", nargs="+")
    p.set_defaults(fn=cmd_build)
    p = sub.add_parser("package")
    p.add_argument("dir")
    p.add_argument("--out", default="dist")
    p.add_argument("--url-base", help="release download URL prefix for artifact_url")
    for flag in ("--author", "--category", "--license", "--source-repo", "--icon-url"):
        p.add_argument(flag)
    p.set_defaults(fn=cmd_package)
    for name, fn in (("run", cmd_run), ("store", cmd_store)):
        p = sub.add_parser(name)
        p.add_argument("rest", nargs=argparse.REMAINDER)
        p.set_defaults(fn=fn)
    args = parser.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
