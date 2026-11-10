#!/usr/bin/env bash
# Prepare the official Ubuntu mirrors in a disposable CI container. The minimal
# ubuntu:20.04 image has no CA certificates, so bootstrap them before requiring
# verified HTTPS for all subsequent package downloads.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
sed -i -E 's#http://(archive[.]ubuntu[.]com/ubuntu|security[.]ubuntu[.]com/ubuntu|ports[.]ubuntu[.]com/ubuntu-ports)#https://\1#g; s#://security[.]ubuntu[.]com/ubuntu#://archive.ubuntu.com/ubuntu#g' /etc/apt/sources.list
APT_OPTIONS=(
    -o APT::Update::Error-Mode=any
    -o Acquire::Retries=3
    -o Acquire::http::Timeout=30
    -o Acquire::https::Timeout=30
)
if ! dpkg-query -W -f='${Status}' ca-certificates 2>/dev/null | grep -qx 'install ok installed'; then
    # Repository metadata and package hashes are still authenticated by APT.
    # This exception is scoped to the CA bootstrap, never to application packages.
    apt-get update "${APT_OPTIONS[@]}" -o Acquire::https::Verify-Peer=false
    apt-get install -y --no-install-recommends "${APT_OPTIONS[@]}" \
        -o Acquire::https::Verify-Peer=false ca-certificates
fi
apt-get update "${APT_OPTIONS[@]}"
