# The Linux smoke-test environment of the Microsoft.JavaScript.LibNode
# packages (scripts/libnode-build.ts --smoke): Ubuntu 22.04 with only its own
# compilers and C++ runtime, the .NET 10 SDK, and Node.js 24 for the script.
# libnode.so must load here with nothing from the build image's newer
# toolchain.
#
# The Linux agent images build it (.ado/image/linux-image-provision.sh),
# tagged with this file's hash, and scripts/libnode-docker.sh runs it with
# --network=none. A change here needs an agent image rebuild.
FROM mcr.microsoft.com/mirror/docker/library/ubuntu:22.04@sha256:104ae83764a5119017b8e8d6218fa0832b09df65aae7d5a6de29a85d813da2fb

ARG DEBIAN_FRONTEND=noninteractive

# g++ builds the C and C++ consumer; .NET needs ICU; unzip extracts packages.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl g++ libicu70 unzip xz-utils \
 && rm -rf /var/lib/apt/lists/*

ENV DOTNET_ROOT=/opt/dotnet \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    PATH=/opt/dotnet:$PATH
RUN curl -fsSLo /tmp/dotnet-install.sh https://builds.dotnet.microsoft.com/dotnet/scripts/v1/dotnet-install.sh \
 && bash /tmp/dotnet-install.sh --channel 10.0 --install-dir /opt/dotnet --no-path \
 && rm /tmp/dotnet-install.sh \
 && dotnet --list-sdks

# Node.js 24 runs scripts/libnode-build.ts.
RUN case "$(uname -m)" in \
      x86_64) arch=x64 sha=4f2b175a6d53a5c617a80dba2676238a0688cef28c6da4eba35c7d844e66fcfb ;; \
      aarch64) arch=arm64 sha=6e51a2ada00f0011cc266f65d98922b403d7ad88f809ca2aa615a5fe1ce6e86c ;; \
      *) echo "unsupported architecture $(uname -m)"; exit 1 ;; \
    esac \
 && curl -fsSLo /tmp/node.tar.xz "https://nodejs.org/dist/v24.22.3/node-v24.22.3-linux-$arch.tar.xz" \
 && echo "$sha  /tmp/node.tar.xz" | sha256sum -c - \
 && mkdir -p /opt/node \
 && tar -xJf /tmp/node.tar.xz -C /opt/node --strip-components=1 --no-same-owner \
 && rm /tmp/node.tar.xz \
 && /opt/node/bin/node --version
ENV PATH=/opt/node/bin:$PATH
