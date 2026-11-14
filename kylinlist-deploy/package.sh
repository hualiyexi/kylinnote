#!/usr/bin/env bash
# 日常发布入口（release-only）：只校验并投递 CI release 目录，不构建任何镜像。
# 客户端安装包来自 GitHub CI；服务端镜像变化时单独用 kylinlist-deploy/package-images.sh。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -ne 1 || ! -d "$1" ]]; then
    echo "用法: bash kylinlist-deploy/package.sh <release目录>" >&2
    exit 2
fi
exec python -I "$ROOT/kylinlist-deploy/package_release.py" "$1"
