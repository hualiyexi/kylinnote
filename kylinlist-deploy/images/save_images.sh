#!/usr/bin/env bash
# 导出三个服务端镜像。镜像须先由 kylinlist-deploy/package-images.sh 构建；tar 不纳入 Git。
set -euo pipefail
cd "$(dirname "$0")"
VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
    VERSION=$(sed -n 's/^KYLINLIST_IMAGE_VERSION=//p' ../.env.example | head -n 1)
fi
if [[ ! "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "用法: bash save_images.sh <X.Y.Z>" >&2
    exit 2
fi

save() {
    local name="$1"
    local image="$2"
    local tar="${name}-${VERSION}.tar"
    if [[ -f "$tar" && "${FORCE:-0}" != "1" ]]; then
        echo "跳过（已存在）: $tar"
    else
        echo "导出 $image -> $tar"
        docker save -o "$tar" "$image"
    fi
}

save kylinlist-http "kylinlist-http:${VERSION}"
save kylinlist-publisher "kylinlist-publisher:${VERSION}"
save kylinlist-stats "kylinlist-stats:${VERSION}"
ls -lh ./*.tar
