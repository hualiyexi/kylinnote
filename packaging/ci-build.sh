#!/usr/bin/env bash
# CI 容器内构建脚本：在 Ubuntu 20.04 arm64 容器中运行（对齐银河麒麟 V10 SP1 基线：
# glibc 2.31 + 系统 Qt 5.12），一次产出常规包与 bundle 离线自包含包。
# 由 .github/workflows/build.yml 调用；本地复现：
#   docker run --platform linux/arm64 -v "$(pwd)":/src -w /src ubuntu:20.04 bash packaging/ci-build.sh
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

echo "==> 安装构建依赖（麒麟源同名包：build-essential cmake qtbase5-dev ...）"
apt-get update || {
    echo "默认 apt 源不可用（focal 已 EOL 下架），切换 old-releases 源重试"
    sed -i 's|http://archive.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g;
            s|http://security.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g;
            s|http://ports.ubuntu.com/ubuntu-ports|http://old-releases.ubuntu.com/ubuntu-ports|g' \
        /etc/apt/sources.list
    apt-get update
}
apt-get install -y --no-install-recommends \
    build-essential cmake qtbase5-dev qtbase5-dev-tools libqt5sql5-sqlite file

echo "==> 构建常规包（依赖目标机系统 Qt，适合预装 Qt 的麒麟机）"
bash packaging/build-deb.sh

echo "==> 构建自包含 bundle 包（Qt 随包携带，离线机 dpkg -i 即用）"
bash packaging/build-deb.sh --bundle-qt

echo "==> 本容器内产物"
ls -lh kylinnote_*_arm64*.deb
