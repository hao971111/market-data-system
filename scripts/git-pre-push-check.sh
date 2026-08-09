#!/usr/bin/env bash
# 本地 git pre-push 门禁：构建并跑 unit tests（tests/unit）。
# 安装：bash scripts/install-git-hooks.sh
# 临时跳过：SKIP_PRE_PUSH_CHECK=1 git push
set -euo pipefail

# 钩子是 .git/hooks/pre-push -> ../../scripts/... 的软链；
# 必须先解析真实路径，否则 dirname "$0" 会落在 .git/hooks，ROOT 会错成 .git/
SCRIPT_PATH="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SCRIPT_PATH")/.." && pwd)"
BUILD_DIR="${MDS_PRE_PUSH_BUILD_DIR:-$ROOT/build-pre-push}"

if [[ "${SKIP_PRE_PUSH_CHECK:-0}" == "1" ]]; then
  echo "[pre-push] SKIP_PRE_PUSH_CHECK=1, skip checks."
  exit 0
fi

echo "[pre-push] configuring ($BUILD_DIR, MDS_BUILD_TESTS=ON)..."
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMDS_BUILD_TESTS=ON \
  -DMDS_BUILD_EXAMPLES=OFF

echo "[pre-push] building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "[pre-push] running ctest..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "[pre-push] PASS"
