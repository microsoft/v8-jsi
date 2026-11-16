#!/usr/bin/env bash
# Runs once while a v8-jsi Linux agent image is built, when the image build
# still has network access. It builds the libnode containers that the
# network-isolated CI jobs run with scripts/libnode-docker.sh:
#   v8jsi-libnode-linux-build  the Ubuntu 22.04 build environment
#   v8jsi-libnode-linux-smoke  the Ubuntu 22.04 smoke-test environment
# Each one is tagged with the first 16 hex digits of its Dockerfile's SHA-256.
#
# The image recipes (azurelinux3-1espt-*.json) carry this script and the two
# Dockerfiles, compressed and base64-encoded, in their linux-bash-command
# entry, because ImageFactory passes that command on a command line where
# quotes and $ would be interpreted. After changing any of them, regenerate
# the recipes with `node scripts/linux-image-recipe.ts` and rebuild both
# images.
#
# Any failure fails the image build, which leaves the previous image version
# in its pool. The output is in the image build log and, in a built image, in
# /opt/v8jsi/image-build-report.txt.
set -euo pipefail

dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p /opt/v8jsi
exec > >(tee /opt/v8jsi/image-build-report.txt) 2>&1

echo "== host"
date -u +%FT%TZ
. /etc/os-release
echo "$PRETTY_NAME, $(uname -m), $(nproc) CPUs"
df -h / /mnt 2> /dev/null || true

echo "== docker"
systemctl is-active --quiet docker || systemctl start docker
docker version --format 'client {{.Client.Version}}, server {{.Server.Version}}'

context=$(mktemp -d)
for kind in build smoke; do
  dockerfile="$dir/libnode-linux-$kind.Dockerfile"
  image="v8jsi-libnode-linux-$kind:$(sha256sum < "$dockerfile" | cut -c1-16)"
  echo "== $image"
  docker build --tag "$image" --file "$dockerfile" "$context"
done
rmdir "$context"

echo "== libnode containers"
docker images 'v8jsi-libnode-linux-*'
