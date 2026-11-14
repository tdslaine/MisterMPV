# Build environment for the misterMPV AppImage: old glibc (Ubuntu 22.04 = glibc 2.35)
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git ca-certificates curl xz-utils file pkg-config nasm cmake ninja-build \
    python3 python3-pip python3-jinja2 patchelf desktop-file-utils \
    libass-dev libluajit-5.1-dev libarchive-dev libjpeg-dev zlib1g-dev libbz2-dev liblzma-dev \
    libgnutls28-dev libdav1d-dev libpulse-dev libasound2-dev libuchardet-dev \
    qt6-base-dev qt6-base-dev-tools qt6-wayland libqt6dbus6 libgl-dev libxkbcommon-dev \
    libfontconfig1-dev libfreetype6-dev libfribidi-dev libharfbuzz-dev \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir meson==1.6.1
