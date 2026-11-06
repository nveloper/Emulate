#!/usr/bin/env bash
# Builds the libretro cores (mGBA for GB/GBC/GBA, melonDS DS for NDS) with the Android NDK
# and copies the stripped .so files into app/src/main/jniLibs/<abi>/.
#
# Usage: scripts/build-cores.sh [abi ...]      (default: arm64-v8a x86_64)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_HOME:-${LOCALAPPDATA:-$HOME/AppData/Local}/Android/Sdk}"
SDK="$(cd "$SDK" && pwd)"
NDK="${ANDROID_NDK:-$SDK/ndk/28.2.13676358}"
CMAKE_DIR="$SDK/cmake/3.22.1/bin"
CMAKE="$CMAKE_DIR/cmake"
NINJA="$CMAKE_DIR/ninja"
[ -x "$CMAKE.exe" ] && CMAKE="$CMAKE.exe" && NINJA="$NINJA.exe"
STRIP="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)/llvm-strip"
[ -x "$STRIP.exe" ] && STRIP="$STRIP.exe"
API=26
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"

ABIS=("$@")
[ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a x86_64)

common_args=(
  -G Ninja
  -DCMAKE_MAKE_PROGRAM="$NINJA"
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
  -DANDROID_PLATFORM="android-$API"
  -DANDROID_STL=c++_static
  -DCMAKE_BUILD_TYPE=Release
)

# mGBA network link cable (cores/mgba-link): add the source files and the hooks in libretro.c.
MGBA="$ROOT/third_party/mgba-libretro"
cp "$ROOT"/cores/mgba-link/libretro-link.[ch] "$MGBA/src/platform/libretro/"
if git -C "$MGBA" apply --check "$ROOT/patches/mgba-link.patch" 2>/dev/null; then
  git -C "$MGBA" apply "$ROOT/patches/mgba-link.patch"
elif ! git -C "$MGBA" apply -R --check "$ROOT/patches/mgba-link.patch" 2>/dev/null; then
  echo "patches/mgba-link.patch does not apply to third_party/mgba-libretro" >&2
  exit 1
fi

for ABI in "${ABIS[@]}"; do
  echo "=== $ABI: mGBA ==="
  B="$ROOT/build-cores/mgba-$ABI"
  "$CMAKE" -S "$ROOT/third_party/mgba-libretro" -B "$B" "${common_args[@]}" -DANDROID_ABI="$ABI" \
    -DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=OFF \
    -DDISABLE_DEPS=ON -DUSE_EPOXY=OFF -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF \
    -DENABLE_DEBUGGERS=OFF -DENABLE_GDB_STUB=OFF -DUSE_EDITLINE=OFF -DENABLE_SCRIPTING=OFF -DUSE_DISCORD_RPC=OFF >/dev/null
  "$CMAKE" --build "$B" -j "$JOBS" >/dev/null

  echo "=== $ABI: melonDS DS ==="
  B="$ROOT/build-cores/melondsds-$ABI"
  "$CMAKE" -S "$ROOT/third_party/melonds-ds" -B "$B" "${common_args[@]}" -DANDROID_ABI="$ABI" >/dev/null
  "$CMAKE" --build "$B" -j "$JOBS" >/dev/null

  OUT="$ROOT/app/src/main/jniLibs/$ABI"
  mkdir -p "$OUT"
  "$STRIP" --strip-unneeded -o "$OUT/libmgba_libretro.so" "$ROOT/build-cores/mgba-$ABI/mgba_libretro.so"
  "$STRIP" --strip-unneeded -o "$OUT/libmelondsds_libretro.so" "$ROOT/build-cores/melondsds-$ABI/src/libretro/melondsds_libretro_android.so"
  ls -la "$OUT"
done
