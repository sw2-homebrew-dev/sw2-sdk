#!/bin/sh
# Build and run the SDK runtime tests on the host and under qemu-aarch64.
set -e
cd "$(dirname "$0")/../.."
OUT=build/tests
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Werror -Isdk/include -isystem sdk/third_party/vulkan-headers/include"
SOURCES="tests/abi/runtime_test.c tests/abi/harness.c sdk/src/hbx_app.c"
HOSTCC=${HOSTCC:-cc}
eval "$(python3 sdk/tools/toolchain.py --sh)"

echo "== host x86-64 =="
$HOSTCC $CFLAGS -o "$OUT/runtime_test_host" $SOURCES tests/abi/entry_host.c
"$OUT/runtime_test_host"

if ! command -v qemu-aarch64 >/dev/null 2>&1; then
  echo "== qemu-aarch64: SKIPPED (not installed; Linux only) =="
  exit 0
fi
echo "== qemu-aarch64 ($TOOLCHAIN_KIND) =="
$CC -std=gnu11 -O2 -ffreestanding -nostdlib -fno-builtin \
  -fno-pic -fno-pie -mcmodel=large -mbranch-protection=bti -march=armv8-a \
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -Wall -Wextra -Werror -DHBX_TEST_NO_SLEEP -Isdk/include -isystem sdk/third_party/vulkan-headers/include \
  -Wl,-e,_start -static \
  -o "$OUT/runtime_test_aarch64" $SOURCES sdk/src/hbx_math.c tests/abi/entry_aarch64.S tests/abi/report_aarch64.c
qemu-aarch64 "$OUT/runtime_test_aarch64"
echo "qemu-aarch64 runtime tests passed"

echo "== entry trampoline (qemu-aarch64) =="
$CC -std=gnu11 -O2 -ffreestanding -nostdlib -fno-builtin \
  -fno-pic -fno-pie -mcmodel=large -mbranch-protection=bti -march=armv8-a \
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -Wall -Wextra -Werror -DHBX_TEST_NO_SLEEP -Isdk/include -isystem sdk/third_party/vulkan-headers/include \
  -Wl,-e,_boot -static -o "$OUT/entry_test_aarch64" \
  tests/abi/entry_test.c tests/abi/entry_test.S sdk/src/crt0.S \
  sdk/src/hbx_app.c sdk/src/hbx_math.c tests/abi/entry_test_main.S
qemu-aarch64 "$OUT/entry_test_aarch64"
echo "entry trampoline test passed"
