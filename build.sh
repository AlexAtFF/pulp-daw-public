#!/usr/bin/env bash
# PULP on Linux (platform/linux/README.md):
#   ./build.sh         everything: builds, runs the tests, fills dist/PULP-linux
#   ./build.sh quick   engine, renderer and tests only (no JUCE, no window), runs the tests
#   ./build.sh app     the program and the renderer, fills dist/PULP-linux
set -euo pipefail
cd "$(dirname "$0")"
JOBS="${JOBS:-$(nproc)}"
configure() { cmake -S . -B "$1" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/platform/linux/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release "${@:2}"; }
gui_deps() {
  local p m=()
  for p in alsa freetype2 fontconfig x11 xext xi xinerama xrandr xcursor xcomposite xrender; do pkg-config --exists "$p" || m+=("$p"); done
  if ((${#m[@]})); then echo "build.sh: missing development files for: ${m[*]} (platform/linux/README.md lists the packages)"; exit 1; fi
}
fill_dist() { # the program, the renderer and the factory folder side by side, like dist\PULP on Windows
  local app; app=$(find "$1" -path '*pulp_app_artefacts*' -type f -name PULP -perm -u+x | head -1)
  [ -n "$app" ] || { echo "build.sh: the program was not built"; exit 1; }
  mkdir -p dist/PULP-linux/factory
  cp "$app" "$1/pulp_render" dist/PULP-linux/
  cp -a factory/. dist/PULP-linux/factory/
  echo "Built dist/PULP-linux/PULP"
}
case "${1:-}" in
  quick) configure build/linux-quick -DPULP_BUILD_APP=OFF
         cmake --build build/linux-quick -j "$JOBS"
         build/linux-quick/pulp_tests ;;
  app)   gui_deps; configure build/linux -DPULP_BUILD_APP=ON
         cmake --build build/linux --target pulp_app pulp_render -j "$JOBS"
         fill_dist build/linux ;;
  "")    gui_deps; configure build/linux -DPULP_BUILD_APP=ON
         cmake --build build/linux -j "$JOBS"
         build/linux/pulp_tests
         if [ ! -d factory/Instruments ]; then # the factory instruments, built once, as build.bat does
           build/linux/pulp_render --make-factory factory
           python3 -I tools/fetch-samples.py || true
           build/linux/pulp_render --build-library factory/sources.json factory-cache factory
           python3 -I tools/make_credits.py || true
         fi
         fill_dist build/linux ;;
  *)     sed -n 2,5p "$0"; exit 1 ;;
esac
