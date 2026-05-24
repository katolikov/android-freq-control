#!/usr/bin/env bash
# Build the FreqControl static library and the freq_ctl CLI for Android.
#
# All SoCs declared under include/freq_control/soc/ are compiled in; the
# active device is picked at runtime via SetActiveDevice or the freq_ctl
# `--device <name>` flag. There is no build-time SoC selection.
#
# Requires the Android NDK. The script picks one in this order:
#   1. $ANDROID_NDK
#   2. $ANDROID_NDK_HOME
#   3. highest-versioned dir under $ANDROID_HOME/ndk/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

build_dir="build"
abi="arm64-v8a"
api="30"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
clean=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -b, --build-dir <dir>   Out-of-tree build directory (default: $build_dir).
  -c, --clean             Remove the build dir before configuring.
  -j, --jobs <N>          Parallel build jobs (default: $jobs).
      --abi <abi>         Android ABI (default: $abi).
      --api <level>       Android platform API (default: $api).
  -h, --help              Show this help.

Required environment: ANDROID_NDK (or ANDROID_NDK_HOME, or ANDROID_HOME/ndk/).

To add a new device:
    python3 tools/gen_device_config.py --adb-serial <SERIAL>
then rebuild. The new SoC is appended to the runtime registry and selected
at runtime via SetActiveDevice / SetActiveDeviceByName.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--build-dir)  build_dir="$2"; shift 2 ;;
    -c|--clean)      clean=1; shift ;;
    -j|--jobs)       jobs="$2"; shift 2 ;;
    --abi)           abi="$2"; shift 2 ;;
    --api)           api="$2"; shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

ndk="${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}"
if [[ -z "$ndk" && -d "${ANDROID_HOME:-}/ndk" ]]; then
  ndk="$(find "$ANDROID_HOME/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)"
fi
if [[ -z "$ndk" || ! -d "$ndk" ]]; then
  cat >&2 <<EOF
error: cannot find Android NDK.
  Set ANDROID_NDK to your NDK install path, e.g.:
    export ANDROID_NDK=\$HOME/Library/Android/sdk/ndk/27.0.12077973
EOF
  exit 1
fi

toolchain="$ndk/build/cmake/android.toolchain.cmake"
if [[ ! -f "$toolchain" ]]; then
  echo "error: cmake toolchain file not found at $toolchain" >&2
  exit 1
fi

registry="$SCRIPT_DIR/include/freq_control/device_registry.h"
if [[ ! -f "$registry" ]]; then
  cat >&2 <<EOF
error: device registry missing: $registry
  generate it first:
    python3 tools/gen_device_config.py --adb-serial <SERIAL>
EOF
  exit 1
fi

cd "$SCRIPT_DIR"

if (( clean )) && [[ -d "$build_dir" ]]; then
  echo "cleaning $build_dir"
  rm -rf "$build_dir"
fi

echo "configuring:"
echo "  NDK         $ndk"
echo "  ABI         $abi"
echo "  API         $api"
echo "  build dir   $build_dir"
echo

cmake -S . -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DANDROID_ABI="$abi" \
  -DANDROID_PLATFORM="android-$api"

echo
echo "building (-j$jobs)"
cmake --build "$build_dir" -- -j"$jobs"

echo
echo "outputs:"
ls -lh "$build_dir/freq_ctl" "$build_dir/libfreq_control.a" 2>/dev/null
echo
echo "next: adb push $build_dir/freq_ctl /data/local/tmp/  (or use ./run.py)"
