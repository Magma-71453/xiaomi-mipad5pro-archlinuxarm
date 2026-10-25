#!/bin/sh
# Build qbootctl (A/B slot switch helper for Qualcomm devices) and install it.
#
# Upstream lives on GitLab, which is behind Cloudflare and refuses plain curl,
# so fetch the official GitHub mirror instead.
set -e
SRC=${SRC:-/tmp/qbootctl-build}
rm -rf "$SRC"
mkdir -p "$SRC"
cd "$SRC"
curl -sfL https://codeload.github.com/linux-msm/qbootctl/tar.gz/refs/heads/main -o q.tgz
tar xzf q.tgz --strip-components=1
meson setup build --buildtype=release
ninja -C build
sudo install -m755 build/qbootctl /usr/local/bin/qbootctl
echo "installed /usr/local/bin/qbootctl"
/usr/local/bin/qbootctl 2>/dev/null || sudo /usr/local/bin/qbootctl | head -8
