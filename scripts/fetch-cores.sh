#!/usr/bin/env bash
# Fetches the pinned emulator core sources into third_party/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/third_party"

fetch() {  # name url commit
  local dir="$ROOT/third_party/$1"
  if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$2"
  fi
  git -C "$dir" fetch -q --depth 1 origin "$3"
  git -C "$dir" checkout -q FETCH_HEAD
  echo "$1 @ $3"
}

# GB / GBC / GBA
fetch mgba-libretro https://github.com/libretro/mgba.git 7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6
# NDS (melonDS DS 1.4.0)
fetch melonds-ds https://github.com/JesseTG/melonds-ds.git f394adbacb5722ee97c1b37c8064da9a25818310
