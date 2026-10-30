#!/bin/sh
# Reproducible build of everything offline: assets, launcher, apps, preview.
set -e
cd "$(dirname "$0")/.."
echo "== assets =="
python3 tools/make_font_atlas.py --skip-missing-fonts
python3 tools/build_shaders.py
python3 tools/gen_abi.py
echo "== launcher =="
sh launcher/build.sh
echo "== apps =="
sh tools/build-apps.sh
echo "== probe =="
python3 sdk/tools/hbx-build.py tests/probe
echo "== GPUBrew runner =="
python3 sdk/tools/hbx-build.py sdk/runners/gpubrew
echo "== SDK example =="
python3 sdk/tools/hbx-build.py sdk/examples/raw-vulkan
echo "== host preview =="
if [ "$(uname -s)" = Linux ]; then
  sh tests/preview/build.sh
else
  sh tests/preview/build.sh || echo "host preview skipped (needs a Vulkan loader; optional off Linux)"
fi
echo "build complete"
