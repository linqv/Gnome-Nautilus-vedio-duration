#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CC_BIN="${CC:-clang}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
NAUTILUS_EXT_DIR="${NAUTILUS_EXT_DIR:-/usr/lib/nautilus/extensions-4}"
PARALLEL="${PARALLEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
NATIVE_OPTIMIZE="${NAUTILUS_VD_NATIVE_OPTIMIZE:-ON}"
LTO="${NAUTILUS_VD_LTO:-ON}"
INSTALL_STRIP="${INSTALL_STRIP:-ON}"

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
  echo "Compiler not found: $CC_BIN" >&2
  exit 1
fi

echo "==> Configuring"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_C_COMPILER="$CC_BIN" \
  -DNAUTILUS_VD_NATIVE_OPTIMIZE="$NATIVE_OPTIMIZE" \
  -DNAUTILUS_VD_LTO="$LTO" \
  -DNAUTILUS_EXT_DIR="$NAUTILUS_EXT_DIR"

echo "==> Building"
cmake --build "$BUILD_DIR" --parallel "$PARALLEL"

echo "==> Installing to $NAUTILUS_EXT_DIR"
install_args=(--install "$BUILD_DIR")
if [[ "$BUILD_TYPE" == "Release" && "$INSTALL_STRIP" == "ON" ]]; then
  install_args+=(--strip)
fi
sudo cmake "${install_args[@]}"

echo "==> Restarting Nautilus"
nautilus -q || true

echo "Install complete."
