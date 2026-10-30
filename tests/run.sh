#!/bin/sh
# Full offline verification.  No console required.
set -e
cd "$(dirname "$0")/.."
NODE=$(command -v node || true)
[ -n "$NODE" ] || NODE=${HBL_NODE:-"$PWD/../tools/node"}
if [ ! -x "$NODE" ] && ! command -v "$NODE" >/dev/null 2>&1; then
  echo "tests/run.sh: node not found; set HBL_NODE" >&2; exit 2
fi

echo "== generated assets are current =="
python3 tools/make_font_atlas.py --check --skip-missing-fonts
python3 tools/build_shaders.py --check
python3 tools/gen_abi.py --check

echo "== SDK runtime (host + qemu-aarch64) =="
sh tests/abi/build.sh

echo "== hardware probe (mock backend) =="
"$NODE" host/probe.cjs --mock

echo "== host lifecycle =="
"$NODE" tests/host/lifecycle.test.cjs

echo "== sw2 extensions (GPUBrew, services, store) =="
"$NODE" tests/host/sw2.test.cjs

echo "== launcher navigation logic =="
mkdir -p build/tests
${HOSTCC:-cc} -std=gnu11 -O2 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -Isdk/include -isystem sdk/third_party/vulkan-headers/include -Isdk/src -Ilauncher/src \
  tests/ui/logic.test.c launcher/src/launcher_logic.c sdk/src/hbx_app.c \
  -o build/tests/logic_test
./build/tests/logic_test

# The reference render uses Mesa lavapipe, which macOS does not have.
if [ "$(uname -s)" = Linux ] || [ "${SW2_PREVIEW:-0}" = 1 ]; then
  echo "== UI geometry + real Vulkan render =="
  sh tests/preview/build.sh
  "$NODE" tests/ui/ui.test.cjs
else
  echo "== UI render: SKIPPED on $(uname -s) (set SW2_PREVIEW=1 with a Vulkan ICD) =="
fi

echo "all offline tests passed"
