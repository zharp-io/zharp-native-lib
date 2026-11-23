# Build, test and crash the SDK on Linux: gcc and clang, CMake, libcurl, Mozilla dump_syms,
# rust-minidump's minidump-stackwalk (the walker Zharp's backend runs) and Node (for the CLI).
#   docker build -t zharp-native-sdk-dev e2e
FROM node:22-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build clang lld libcurl4-openssl-dev zlib1g-dev python3 xz-utils curl ca-certificates file \
    && rm -rf /var/lib/apt/lists/*
RUN cd /tmp && \
    curl -fsSLO https://github.com/mozilla/dump_syms/releases/download/v2.3.9/dump_syms-x86_64-unknown-linux-gnu.tar.xz && \
    curl -fsSLO https://github.com/mozilla/dump_syms/releases/download/v2.3.9/dump_syms-x86_64-unknown-linux-gnu.tar.xz.sha256 && \
    sha256sum -c dump_syms-x86_64-unknown-linux-gnu.tar.xz.sha256 && \
    tar -xJf dump_syms-x86_64-unknown-linux-gnu.tar.xz && find . -name dump_syms -type f -exec install {} /usr/local/bin/dump_syms \; && \
    curl -fsSLO https://github.com/rust-minidump/rust-minidump/releases/download/v0.27.0/minidump-stackwalk-x86_64-unknown-linux-musl.tar.xz && \
    curl -fsSLO https://github.com/rust-minidump/rust-minidump/releases/download/v0.27.0/minidump-stackwalk-x86_64-unknown-linux-musl.tar.xz.sha256 && \
    sha256sum -c minidump-stackwalk-x86_64-unknown-linux-musl.tar.xz.sha256 && \
    tar -xJf minidump-stackwalk-x86_64-unknown-linux-musl.tar.xz && find . -name minidump-stackwalk -type f -exec install {} /usr/local/bin/ \; && \
    rm -rf /tmp/*
WORKDIR /src
