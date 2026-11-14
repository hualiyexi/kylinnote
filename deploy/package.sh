#!/usr/bin/env bash
# 在开发机生成完整离线部署目录：构建/校验/导出镜像，并带上本次安装包与清单。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RELEASE_DIR="${1:-}"
if [[ -z "$RELEASE_DIR" || ! -d "$RELEASE_DIR" ]]; then
    echo "用法: bash deploy/package.sh <release目录> [X.Y.Z]" >&2
    exit 2
fi
VERSION="${2:-}"
if [[ -z "$VERSION" ]]; then
    VERSION=$(sed -n 's/^KYLINLIST_IMAGE_VERSION=//p' "$ROOT/deploy/.env.example" | head -n 1)
fi
if [[ ! "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "版本必须是严格的 X.Y.Z: $VERSION" >&2
    exit 2
fi
PROJECT_VERSION=$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\)[[:space:]]*$/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)
if [[ "$PROJECT_VERSION" != "$VERSION" ]]; then
    echo "版本与 CMakeLists.txt 不一致: $VERSION != $PROJECT_VERSION" >&2
    exit 2
fi

cd "$ROOT"
echo "构建 KylinList $VERSION 部署镜像…"
docker build -f server/Dockerfile.http -t "kylinlist-http:$VERSION" .
docker build -f server/Dockerfile.publisher -t "kylinlist-publisher:$VERSION" .
docker build -f server/stats/Dockerfile -t "kylinlist-stats:$VERSION" server/stats

RELEASE_HOST=$(cd "$RELEASE_DIR" && pwd)
if command -v cygpath >/dev/null 2>&1; then RELEASE_HOST=$(cygpath -m "$RELEASE_HOST"); fi
MSYS_NO_PATHCONV=1 docker run --rm -v "$RELEASE_HOST:/release:ro" \
    "kylinlist-publisher:$VERSION" validate /release

TARGET="$ROOT/deploy/data/incoming/kylinlist-$VERSION"
if [[ -e "$TARGET" ]]; then
    echo "拒绝覆盖已存在的发布目录: $TARGET" >&2
    exit 1
fi
mkdir -p "$TARGET"
cp -R "$RELEASE_DIR/." "$TARGET/"

if [[ ! -f "$ROOT/deploy/.env.prod" ]]; then
    sed "s/^KYLINLIST_IMAGE_VERSION=.*/KYLINLIST_IMAGE_VERSION=$VERSION/" \
        "$ROOT/deploy/.env.example" > "$ROOT/deploy/.env.prod"
fi
bash "$ROOT/deploy/images/save_images.sh" "$VERSION"

cd "$ROOT/deploy"
# 校验清单只覆盖本次版本，避免 images/ 中保留的历史 tar 混入新部署包。
{
    find "data/incoming/kylinlist-$VERSION" -type f -print0
    printf '%s\0' \
        "images/kylinlist-http-$VERSION.tar" \
        "images/kylinlist-publisher-$VERSION.tar" \
        "images/kylinlist-stats-$VERSION.tar"
} | sort -z | xargs -0 sha256sum > SHA256SUMS
echo "部署目录已就绪。请先核对 .env.prod 的镜像版本，再整体拷贝到可信内网生产机。"
