#!/usr/bin/env bash
# 服务端镜像独立打包：仅当 http / publisher / stats 自身变化时使用。
# 与客户端 release 无关：不读取 CMake 版本、不生成 incoming；
# 镜像版本默认取 deploy/.env.example 的 KYLINLIST_IMAGE_VERSION，可传参覆盖。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
    VERSION=$(sed -n 's/^KYLINLIST_IMAGE_VERSION=//p' deploy/.env.example | head -n 1)
fi
if [[ ! "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "用法: bash deploy/package-images.sh [X.Y.Z]（默认取 deploy/.env.example 的 KYLINLIST_IMAGE_VERSION，独立于 CMake 客户端版本）" >&2
    exit 2
fi

echo "构建 KylinList 服务端镜像 $VERSION（http / publisher / stats）…"
docker build -f server/Dockerfile.http -t "kylinlist-http:$VERSION" .
docker build -f server/Dockerfile.publisher -t "kylinlist-publisher:$VERSION" .
docker build -f server/stats/Dockerfile -t "kylinlist-stats:$VERSION" server/stats

bash deploy/images/save_images.sh "$VERSION"

cd "$ROOT/deploy/images"
# 校验清单只含本次三个 tar，避免历史版本 tar 混入（重导同名 tar 可设 FORCE=1）。
printf '%s\n' \
    "kylinlist-http-$VERSION.tar" \
    "kylinlist-publisher-$VERSION.tar" \
    "kylinlist-stats-$VERSION.tar" \
    | sort | xargs sha256sum > SHA256SUMS.tmp
mv -f SHA256SUMS.tmp SHA256SUMS
echo "已生成 deploy/images/SHA256SUMS（仅本次三个 tar）:"
cat SHA256SUMS
ls -lh ./*.tar
