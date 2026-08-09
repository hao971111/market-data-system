#!/usr/bin/env bash
# 把 scripts/git-pre-push-check.sh 安装为本地 .git/hooks/pre-push
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOK_DIR="$ROOT/.git/hooks"
HOOK_PATH="$HOOK_DIR/pre-push"
SCRIPT_PATH="$ROOT/scripts/git-pre-push-check.sh"

if [[ ! -d "$ROOT/.git" ]]; then
  echo "not a git repository: $ROOT" >&2
  exit 1
fi

if [[ ! -x "$SCRIPT_PATH" ]]; then
  chmod +x "$SCRIPT_PATH"
fi

mkdir -p "$HOOK_DIR"
ln -sfn ../../scripts/git-pre-push-check.sh "$HOOK_PATH"
chmod +x "$HOOK_PATH"

echo "installed: $HOOK_PATH -> ../../scripts/git-pre-push-check.sh"
echo "try: git push   (skip with SKIP_PRE_PUSH_CHECK=1 git push)"
