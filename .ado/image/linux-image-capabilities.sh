#!/usr/bin/env bash
# Runs once while a v8-jsi Linux agent image is built, when artifacts still have
# network access. It records what the image build can do for the libnode build
# and leaves a test container in the image. It never fails the image build; the
# agent check pipeline reads /opt/v8jsi/image-build-report.txt.
#
# The image recipes (azurelinux3-1espt-*.json) carry this script base64-encoded
# in their linux-bash-command entry, because ImageFactory passes that command on
# a command line where quotes and $ would be interpreted.

set -u

report_dir=/opt/v8jsi
mkdir -p "$report_dir"
exec > >(tee "$report_dir/image-build-report.txt") 2>&1

section() { printf '\n== %s ==\n' "$1"; }

section host
date -u +%FT%TZ
id
. /etc/os-release
echo "$PRETTY_NAME, $(uname -m), kernel $(uname -r)"
echo "CPUs: $(nproc)"
free -g | sed -n '1,2p'
df -h / /mnt 2>/dev/null

section docker
systemctl is-active --quiet docker || systemctl start docker
docker version --format 'client {{.Client.Version}}, server {{.Server.Version}}' || echo 'docker is unavailable'
docker images --format '{{.Repository}}:{{.Tag}} {{.Size}}'

section network
for url in \
    https://apt.llvm.org/llvm-snapshot.gpg.key \
    https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/dists/jammy/InRelease \
    http://archive.ubuntu.com/ubuntu/dists/jammy/InRelease \
    http://ports.ubuntu.com/ubuntu-ports/dists/jammy/InRelease \
    https://static.rust-lang.org/rustup/release-stable.toml \
    https://nodejs.org/dist/index.json \
    https://builds.dotnet.microsoft.com/dotnet/scripts/v1/dotnet-install.sh; do
  printf '%s  %s\n' "$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 "$url")" "$url"
done

section 'test container'
work=$(mktemp -d)
cat > "$work/Dockerfile" <<'EOF'
FROM mcr.microsoft.com/mirror/docker/library/ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends binutils ca-certificates curl gnupg libc6-dev \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
      | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && echo 'deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main' \
      > /etc/apt/sources.list.d/llvm.list \
 && curl -fsSL 'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x60C317803A41BA51845E371A1E9377A2BA9EF27F' \
      | gpg --dearmor -o /usr/share/keyrings/ubuntu-toolchain-r.gpg \
 && echo 'deb [signed-by=/usr/share/keyrings/ubuntu-toolchain-r.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main' \
      > /etc/apt/sources.list.d/ubuntu-toolchain-r.list \
 && apt-get update \
 && apt-cache policy libstdc++-13-dev libstdc++-13-pic gcc-13 > /opt/apt-policy.txt \
 && apt-get install -y --no-install-recommends clang-20 lld-20 libstdc++-13-dev \
 && rm -rf /var/lib/apt/lists/*
EOF
if docker build -t v8jsi-image-check:1 "$work" > "$report_dir/docker-build.log" 2>&1; then
  echo 'docker build: ok'
  docker run --rm --network=none v8jsi-image-check:1 sh -c \
    'cat /opt/apt-policy.txt; clang-20 --version | head -n 1; clang++-20 -v -E -x c++ /dev/null 2>&1 | grep "Selected GCC"'
else
  echo "docker build: failed; the last lines of $report_dir/docker-build.log follow"
  tail -n 40 "$report_dir/docker-build.log"
fi
rm -rf "$work"
docker images --format '{{.Repository}}:{{.Tag}} {{.Size}}'

section done
exit 0
