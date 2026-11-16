#!/usr/bin/env bash
# Runs a command in one of the libnode Linux containers that the Linux agent
# images provide (.ado/image/linux-image-provision.sh): the Ubuntu 22.04 build
# container or the smoke-test container. The command runs as the calling
# user, with no network, in the repository, which is mounted at its own path,
# as are the folders listed in LIBNODE_DOCKER_MOUNTS (separated by spaces).
#
#   scripts/libnode-docker.sh build|smoke <command> [args...]
set -euo pipefail

kind=${1:?usage: libnode-docker.sh build|smoke <command> [args...]}
shift
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dockerfile="$repo/.ado/image/libnode-linux-$kind.Dockerfile"
if [ ! -f "$dockerfile" ]; then
  echo "Unknown container: $kind (use build or smoke)" >&2
  exit 1
fi

# The agent images tag each container with the first 16 hex digits of the
# SHA-256 of its Dockerfile with LF line endings.
tag=$(tr -d '\r' < "$dockerfile" | sha256sum | cut -c1-16)
image="v8jsi-libnode-linux-$kind:$tag"
if ! docker image inspect "$image" > /dev/null 2>&1; then
  echo "##[error]$image is not on this agent: .ado/image/libnode-linux-$kind.Dockerfile changed after the agent image was built. Rebuild the Linux agent images (.ado/image/azurelinux3-1espt-*.json)." >&2
  echo "libnode containers on this agent:" >&2
  docker images 'v8jsi-libnode-linux-*' >&2
  exit 1
fi

mounts=(--volume "$repo:$repo")
for dir in ${LIBNODE_DOCKER_MOUNTS:-}; do
  mkdir -p "$dir"
  mounts+=(--volume "$dir:$dir")
done

exec docker run --rm --network=none --user "$(id -u):$(id -g)" --env HOME=/tmp \
  "${mounts[@]}" --workdir "$repo" "$image" "$@"
