# The Linux build environment of the Microsoft.JavaScript.LibNode packages
# (scripts/libnode-build.ts). It is Ubuntu 22.04, so libnode.so needs glibc
# 2.35 and runs on Ubuntu 22.04 and later and on other distributions with
# glibc 2.35 or newer.
#
# Ubuntu 22.04's own compilers can't build Node.js 26, which needs Clang 19.1
# or GCC 13.2 and libstdc++ 13 (V8 uses std::format). Clang comes from LLVM's
# apt repository and libstdc++ 13 from the Ubuntu Toolchain PPA. The build
# links that C++ runtime into libnode.so with its symbols hidden
# (src/libnode_settings.gypi), so the library needs no newer libstdc++ than
# the system has.
#
# CI jobs run under network isolation, so the image has everything the build
# needs. The Linux agent images build it (.ado/image/linux-image-provision.sh),
# tagged with this file's hash, and scripts/libnode-docker.sh runs it with
# --network=none. A change here needs an agent image rebuild.
FROM mcr.microsoft.com/mirror/docker/library/ubuntu:22.04@sha256:104ae83764a5119017b8e8d6218fa0832b09df65aae7d5a6de29a85d813da2fb

ARG DEBIAN_FRONTEND=noninteractive

# The LLVM and Ubuntu Toolchain PPA signing keys are checked by fingerprint.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
      | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && curl -fsSL 'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x60C317803A41BA51845E371A1E9377A2BA9EF27F' \
      | gpg --dearmor -o /usr/share/keyrings/ubuntu-toolchain-r.gpg \
 && gpg --show-keys --with-colons /usr/share/keyrings/llvm.gpg \
      | grep -q '^fpr:*6084F3CF814B57C1CF12EFD515CF4D18AF4F7421:' \
 && gpg --show-keys --with-colons /usr/share/keyrings/ubuntu-toolchain-r.gpg \
      | grep -q '^fpr:*60C317803A41BA51845E371A1E9377A2BA9EF27F:' \
 && echo 'deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main' \
      > /etc/apt/sources.list.d/llvm.list \
 && echo 'deb [signed-by=/usr/share/keyrings/ubuntu-toolchain-r.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main' \
      > /etc/apt/sources.list.d/ubuntu-toolchain-r.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils clang-20 libc6-dev libstdc++-13-dev libstdc++-13-pic make python3 xz-utils \
 && rm -rf /var/lib/apt/lists/* \
 && update-alternatives --install /usr/bin/cc cc /usr/bin/clang-20 100 \
 && update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-20 100

# Clang builds Node.js; cc, which Rust links its build scripts with, is Clang too.
ENV CC=clang-20 CXX=clang++-20

# Rust for Temporal (deps/crates). CI runs the container as the agent's user,
# so the toolchain is writable by every user, as in the official Rust images.
ENV RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo PATH=/opt/rust/cargo/bin:$PATH
RUN case "$(uname -m)" in \
      x86_64) sha=b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55 ;; \
      aarch64) sha=9f608c142e0e38a78957d0661ba1dfaf0b6fb37184cfaa4b9a0aa57af6184282 ;; \
      *) echo "unsupported architecture $(uname -m)"; exit 1 ;; \
    esac \
 && curl -fsSLo /tmp/rustup-init "https://static.rust-lang.org/rustup/archive/1.29.3/$(uname -m)-unknown-linux-gnu/rustup-init" \
 && echo "$sha  /tmp/rustup-init" | sha256sum -c - \
 && chmod +x /tmp/rustup-init \
 && /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain 1.100.0 \
 && rm /tmp/rustup-init \
 && chmod -R a+w /opt/rust \
 && rustc --version

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
