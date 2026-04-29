#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
NAUTILUS_EXT_DIR="${NAUTILUS_EXT_DIR:-/usr/lib/nautilus/extensions-4}"

manifest="$BUILD_DIR/install_manifest.txt"

if [[ -f "$manifest" ]]; then
  echo "==> Removing files from install manifest"
  while IFS= read -r path; do
    [[ -n "$path" ]] || continue
    if [[ -e "$path" || -L "$path" ]]; then
      echo "Removing $path"
      sudo rm -f "$path"
    fi
  done < "$manifest"
else
  echo "==> install_manifest.txt not found, removing known installed files"
  sudo rm -f \
    "$NAUTILUS_EXT_DIR/nautilus-video-duration.so" \
    "$NAUTILUS_EXT_DIR/vd-ffmpeg-helper"
fi

echo "==> Restarting Nautilus"
nautilus -q || true

echo "Uninstall complete."
