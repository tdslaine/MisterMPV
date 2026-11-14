#!/bin/bash
# Runs inside the container: builds ffmpeg and libplacebo into /work/deps (cached between runs).
set -euo pipefail
D=/work/deps; mkdir -p $D /work/tmp; cd /work/tmp
if [ ! -f $D/lib/libavcodec.so ]; then
  rm -rf ffmpeg-7.1.2 && tar xf /work/dl/ffmpeg-7.1.2.tar.xz && cd ffmpeg-7.1.2
  ./configure --prefix=$D --enable-shared --disable-static --disable-doc --disable-ffplay \
      --disable-debug --disable-libxcb --disable-xlib --disable-sdl2 --disable-vaapi --disable-vdpau --enable-gnutls --enable-libdav1d --extra-ldflags='-Wl,-rpath,$$ORIGIN/../lib'
  make -j"$(nproc)" && make install && cd ..
fi
if [ ! -f $D/lib/x86_64-linux-gnu/libplacebo.so ] && [ ! -f $D/lib/libplacebo.so ]; then
  rm -rf pl-build
  meson setup pl-build /work/dl/libplacebo --prefix=$D --buildtype=release -Dvulkan=disabled -Dopengl=disabled \
      -Dd3d11=disabled -Ddemos=false -Dtests=false -Dlcms=disabled -Dshaderc=disabled -Dglslang=disabled \
      -Dlibdovi=disabled -Dxxhash=disabled -Dunwind=disabled
  meson install -C pl-build
fi
if [ ! -x /work/zig/zig ]; then mkdir -p /work/zig && tar xf /work/dl/zig-x86_64-linux-0.15.2.tar.xz -C /work/zig --strip-components=1; fi
echo DEPS-OK
