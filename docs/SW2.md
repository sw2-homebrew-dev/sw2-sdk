# sw2-sdk extensions

sw2-sdk is HBLauncher (RavensClawX, MIT) with additions for a wider homebrew
ecosystem. Everything HBLauncher does still works unchanged: ABI 3, the
launcher, native packages, `nes`/`sm64` compatibility packages, the Linux
personality runtime, save blocks, SDK imports, exclusive display, mock and
preview backends. The additions are listed below. Each one is additive, and
ABI 3 apps built against upstream HBLauncher load without changes.

| Addition | Where |
| --- | --- |
| `sw2` CLI (doctor/new/build/package/run/store) | `sw2`, `tools/sw2.py` |
| GPUBrew ABI 1 shader apps (`kind: gpubrew`) | `sdk/runners/gpubrew`, `sdk/gpubrew`, `host/lib/packages.cjs` |
| Compute entries in the Vulkan dispatch table | `sdk/include/hbx_vk.h` (appended, optional) |
| Host services mailbox (store, info) | `sdk/include/hbx_svc.h`, `host/lib/services.cjs` |
| Package manager + desktop store | `host/lib/pkgmgr.cjs`, `host/store-cli.cjs` |

## Building on Linux and macOS

Native images are freestanding AArch64, built with either toolchain:

| Host | Install | Toolchain |
| --- | --- | --- |
| Debian/Ubuntu/Mint | `sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu glslang-tools spirv-tools python3-pil` | `gnu` (default when present) |
| Any Linux | `clang lld llvm` packages | `llvm` |
| macOS (Apple silicon or Intel) | `brew install node python llvm lld glslang spirv-tools` and `pip3 install Pillow` | `llvm` (Homebrew's keg-only paths are found automatically) |

`sdk/tools/toolchain.py` picks the toolchain, and `SW2_TOOLCHAIN=gnu|llvm`
forces one. `./sw2 doctor` shows which one was chosen. CI builds and tests
with GNU and LLVM on Linux, and with LLVM on macOS. The runtime and the crt0
entry trampoline are tested under qemu with both toolchains.

The Vulkan headers are vendored in `sdk/third_party/vulkan-headers`
(Apache-2.0), so every host compiles against the same version.

macOS has no `qemu-aarch64` user mode and no lavapipe, so `tests/run.sh`
skips those two steps there and runs everything else. The font atlas is
committed. Install DejaVu (`brew install --cask font-dejavu`) only if you
want to regenerate it.

> GNU builds are what has run on hardware so far. Treat LLVM-built
> candidates as a separate variable until a hardware cycle has passed with
> one: freeze them with an `SW2_LABEL` that says `llvm`.

## The `sw2` CLI

```sh
./sw2 doctor                              # what's missing from the toolchain
./sw2 new apps/my-app "My App"            # native C app (HBLauncher template)
./sw2 new apps/my-shader "Shader" --gpubrew
./sw2 build apps/my-app
./sw2 package apps/my-app --url-base https://github.com/me/my-app/releases/download/v1.0.0
./sw2 run --mock
./sw2 store list | install <id> | update | remove <id> | refresh
```

`package` writes `dist/<id>-<version>.zip` (deterministic), `.sha256`, and
`<id>.catalog.json`, which is ready to submit to the catalog.

## GPUBrew shader apps

A folder that has `app.json` (`"format": "hblauncher.gpu-app", "abi": 1`) and
no `manifest.json` is treated as a GPUBrew app. The host validates the
SPIR-V and turns the package into a launch of the native **GPUBrew runner**.
The runner is an ordinary ABI 3 app that recreates the GPUBrew contract on
the borrowed display:

- 8 MiB host-coherent storage buffer at set 0 / binding 0, all stages.
- `initialState` → bytes 0..1023, `data` → byte 1024 onward.
- One `(1,1,1)` compute dispatch per frame, or `computeSequence` (begin, then
  `repeat` × compute/after with barriers).
- A full-screen 3-vertex draw with the app's vertex/fragment shaders.
- Optional private `memory` at binding 1, with `deflate` initial images.
- `raw[0..3]` (nn::hid NpadState layout) and `touch[]` rewritten every frame
  from the SDK's merged input, so every pad source works.
- Exit when `reserved.z` (byte 1016) is nonzero, or on host stop.

The runner does not bridge these yet: the hardware extension block at
`0x7f0000` (motion, keyboard, mouse, Joy-Con optical, NFC, UDP, FTP) and audio
effects. Apps that check `hbAvailable()` fall back to pad and touch input.
The loader records these gaps as package warnings.

New shader apps should include `sdk/gpubrew/include/gpubrew.glsl`. This is a
clean-room header for the same byte layout. GPUBrewLauncher has no license
assigned, so none of its files are vendored here.

## Host services mailbox

Native code cannot use the network or the PC's app library. The host can. An
app that declares

```json
"services": ["store"]
```

receives a 64 KiB `struct hbx_svc` at `ctx->args[5]`. To make a call, fill
in a request and return `HBX_SVC_CALL`. The host runs the operation on the
workstation and re-enters the app with `HBX_FLAG_RESUME` and the reply. The
`scratch` area persists across calls. Code, data and heap are mapped fresh on
each entry. This follows the same bounded-entry model the launcher already
uses.

| Op | Service | Request | Reply |
| --- | --- | --- | --- |
| `INFO` | info | - | `key=value` text |
| `CATALOG` | store | `a0` = first row | `hbx_svc_app[]`, `r0` total, `r1` rows |
| `REFRESH` | store | `a0` = first row | as CATALOG, after re-downloading |
| `INSTALL` | store | app id text | message text |
| `REMOVE` | store | app id text | message text |

After any install or remove, the launcher catalog is rescanned once the app
exits.

## Package manager

`host/lib/pkgmgr.cjs` installs in this order: download, then the `sha256`
from the catalog, then unpacking into a staging directory, then the
loader's own validation, then an atomic swap into `apps/<id>`. Only folders
it installed (marked by `.sw2-install.json`) are updated or removed, so
development apps in `apps/` are never touched.

| Variable | Default |
| --- | --- |
| `SW2_CATALOG_URL` | the GitHub Pages `index.json` of sw2-homebrew-catalog |
| `SW2_CACHE` | `~/.cache/sw2` |
| `HBL_APPS` | `apps/` |
