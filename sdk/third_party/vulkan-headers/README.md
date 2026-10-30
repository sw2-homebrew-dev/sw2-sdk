# Vulkan-Headers (vendored)

Khronos Vulkan headers, `VK_HEADER_VERSION` 275, Apache-2.0
(`SPDX-License-Identifier: Apache-2.0` in each file; full text:
https://www.apache.org/licenses/LICENSE-2.0).

Vendored so freestanding builds are identical on every host: bare-metal
clang (`--target=aarch64-none-elf`, macOS) has no system include path, and
Debian's cross GCC otherwise picks up whatever `libvulkan-dev` the PC has.
Only the C headers are included. Update by copying a newer
https://github.com/KhronosGroup/Vulkan-Headers `include/` tree.
