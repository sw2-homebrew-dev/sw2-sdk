#!/usr/bin/env python3
"""Pick the AArch64 cross toolchain for freestanding SDK images.

  gnu   aarch64-linux-gnu-gcc/objcopy/nm/size (Debian/Ubuntu/Mint packages)
  llvm  clang --target=aarch64-none-elf + ld.lld + llvm-objcopy/nm/size
        (Linux packages, or Homebrew `llvm` + `lld` on macOS)

SW2_TOOLCHAIN=gnu|llvm forces one; otherwise gnu is preferred when present
because it is what the qualified console builds used.

  toolchain.py          print the selection
  toolchain.py --sh     print CC/OBJCOPY/NM/SIZE/LIBGCC assignments for sh eval
"""
import os
import shlex
import shutil
import sys

# Homebrew installs llvm and lld keg-only; look there as well as PATH.
EXTRA_DIRS = [
    "/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin",
    "/usr/local/opt/llvm/bin", "/usr/local/opt/lld/bin",
]


def find(name):
    found = shutil.which(name)
    if found:
        return found
    for directory in EXTRA_DIRS:
        candidate = os.path.join(directory, name)
        if os.access(candidate, os.X_OK):
            return candidate
    return None


def gnu():
    tools = {k: find("aarch64-linux-gnu-" + k) for k in ("gcc", "objcopy", "nm", "size")}
    if not all(tools.values()):
        return None
    return {"kind": "gnu", "cc": [tools["gcc"]], "objcopy": tools["objcopy"],
            "nm": tools["nm"], "size": tools["size"], "libs": ["-lgcc"]}


def llvm():
    clang, lld = find("clang"), find("ld.lld")
    tools = {k: find("llvm-" + k) for k in ("objcopy", "nm", "size")}
    if not (clang and lld and all(tools.values())):
        return None
    # Apple's /usr/bin/clang cannot target ELF linking; it still compiles fine,
    # and --ld-path points it at (Homebrew's) ld.lld explicitly.
    return {"kind": "llvm",
            "cc": [clang, "--target=aarch64-none-elf", "-fuse-ld=lld", f"--ld-path={lld}",
                   "-Wno-unused-command-line-argument"],
            "objcopy": tools["objcopy"], "nm": tools["nm"], "size": tools["size"],
            # No compiler-rt for bare aarch64, and none is needed: the SDK and
            # launcher link cleanly without libgcc helpers.
            "libs": []}


def select():
    forced = os.environ.get("SW2_TOOLCHAIN", "").lower()
    order = {"gnu": [gnu], "llvm": [llvm]}.get(forced, [gnu, llvm])
    for pick in order:
        chosen = pick()
        if chosen:
            return chosen
    return None


HINT = ("no AArch64 toolchain found. Debian/Ubuntu/Mint: sudo apt install gcc-aarch64-linux-gnu "
        "binutils-aarch64-linux-gnu  (or clang lld llvm).  macOS: brew install llvm lld")


def require():
    chosen = select()
    if not chosen:
        raise SystemExit("toolchain: " + HINT)
    return chosen


def main():
    chosen = require()
    if "--sh" in sys.argv:
        print(f"CC={shlex.quote(shlex.join(chosen['cc']))}")
        for key in ("objcopy", "nm", "size"):
            print(f"{key.upper()}={shlex.quote(chosen[key])}")
        print(f"LIBGCC={shlex.quote(' '.join(chosen['libs']))}")
        print(f"TOOLCHAIN_KIND={chosen['kind']}")
    else:
        print(f"{chosen['kind']}: {' '.join(chosen['cc'])}")


if __name__ == "__main__":
    main()
