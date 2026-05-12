#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build-install-lib"
PREFIX_DIR="$ROOT/_install"

echo "[install-lib] configure..."
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DMDS_BUILD_LIBRARY=ON \
  -DMDS_BUILD_CLI=OFF \
  -DMDS_BUILD_EXAMPLES=OFF

echo "[install-lib] build..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "[install-lib] install to $PREFIX_DIR ..."
cmake --install "$BUILD_DIR" --prefix "$PREFIX_DIR"

echo "[install-lib] done"
echo "[install-lib] headers: $PREFIX_DIR/include/mds"
echo "[install-lib] library: $PREFIX_DIR/lib/libmds_core.a"
