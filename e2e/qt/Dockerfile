# The Qt layer end to end on Linux: Debian trixie's Qt 6.8 and Qt 5.15 from the distro packages,
# Xvfb, plus what the Native e2e has (CMake, gcc, libcurl, Mozilla dump_syms, rust-minidump's
# minidump-stackwalk, Node for the stand-in API and the CLI).
#   docker build -t zharp-native-sdk-qt e2e/qt
FROM node:22-trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build g++ libcurl4-openssl-dev zlib1g-dev python3 xz-utils curl ca-certificates file \
      qt6-base-dev qt6-declarative-dev qml6-module-qtquick qml6-module-qtquick-controls \
      qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qtqml-workerscript \
      qml6-module-qtquick-layouts \
      qtbase5-dev qt5-qmake qtdeclarative5-dev qml-module-qtquick2 qml-module-qtquick-controls2 \
      qml-module-qtquick-window2 qml-module-qtquick-templates2 \
      xvfb xauth libgl1-mesa-dri fonts-dejavu-core \
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
ENV LANG=C.UTF-8
WORKDIR /src
