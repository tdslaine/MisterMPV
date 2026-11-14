#!/bin/bash
# Runs inside the container: builds the groovy zig lib, mpv and the UI from /work/src and packs the AppImage.
set -euo pipefail
D=/work/deps; S=/work/src; B=/work/build; VER=${VER:-0.2.1}
export PKG_CONFIG_PATH=$D/lib/pkgconfig:$D/lib/x86_64-linux-gnu/pkgconfig
export LD_LIBRARY_PATH=$D/lib:$D/lib/x86_64-linux-gnu
rm -rf $B && mkdir -p $B
# groovy-mister-zig with zig 0.15.2 (fresh, not the host-built .a)
rm -rf $S/third_party/groovy-mister-zig/zig-out $S/third_party/groovy-mister-zig/.zig-cache
(cd $S/third_party/groovy-mister-zig && /work/zig/zig build -Doptimize=ReleaseFast --global-cache-dir /work/home/zig-cache \
    --prefix $S/third_party/groovy-mister-zig/zig-out)
meson setup $B/mpv $S --prefix=/usr --buildtype=release -Dlua=luajit -Dlibmpv=false -Dmanpage-build=disabled \
    -Dtests=false -Dpipewire=disabled -Dx11=disabled -Dwayland=disabled -Dgl=disabled -Dvulkan=disabled \
    -Degl=disabled -Ddrm=disabled -Dgbm=disabled -Dvaapi=disabled -Dvdpau=disabled -Dcdda=disabled \
    -Ddvdnav=disabled -Dlibbluray=disabled -Djavascript=disabled -Dsdl2-audio=disabled -Dsdl2-video=disabled \
    -Dsdl2-gamepad=disabled -Djack=disabled -Dopenal=disabled -Dcaca=disabled -Dsixel=disabled -Dlcms2=disabled \
    -Dzimg=disabled -Drubberband=disabled -Dvapoursynth=disabled -Dshaderc=disabled -Dspirv-cross=disabled \
    -Dpulse=enabled -Dalsa=enabled -Dlibarchive=enabled -Djpeg=enabled -Duchardet=enabled
meson compile -C $B/mpv
cmake -S $S/mister-ui -B $B/ui -DCMAKE_BUILD_TYPE=Release
cmake --build $B/ui -j"$(nproc)"
A=$B/AppDir; mkdir -p $A/usr/bin $A/usr/share/doc/misterMPV
cp $B/mpv/mpv $B/ui/misterMPV $D/bin/ffprobe $A/usr/bin/
cp $S/LICENSE.GPL $S/LICENSE.LGPL $S/Copyright $S/CREDITS.md $A/usr/share/doc/misterMPV/
cp $S/third_party/groovy-mister-zig/LICENSE $A/usr/share/doc/misterMPV/LICENSE-groovy-mister-zig
cp $S/third_party/groovy-mister-zig/zig-pkg/lz4-*/LICENSE $A/usr/share/doc/misterMPV/LICENSE-zig-lz4
cp /work/tmp/ffmpeg-7.1.2/COPYING.LGPLv2.1 $A/usr/share/doc/misterMPV/LICENSE-ffmpeg-LGPL-2.1
cp /work/dl/libplacebo/LICENSE $A/usr/share/doc/misterMPV/LICENSE-libplacebo
cp $S/etc/mpv.svg /work/tmp/mistermpv.svg
cd $B; export PATH=/work/dl:$PATH
export QMAKE=/usr/lib/qt6/bin/qmake EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqoffscreen.so" \
       APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=0 LINUXDEPLOY_OUTPUT_VERSION=$VER
/work/dl/linuxdeploy-x86_64.AppImage --appdir $A -e $A/usr/bin/misterMPV -e $A/usr/bin/mpv -e $A/usr/bin/ffprobe \
    -l /usr/lib/x86_64-linux-gnu/libfribidi.so.0 -d /work/misterMPV.desktop -i /work/tmp/mistermpv.svg --plugin qt
# Wayland: linuxdeploy-plugin-qt only takes the platform plugin; without the shell integration (xdg-shell),
# decorations and wayland-egl, Qt cannot open a window under Wayland. Copy the client-side plugins and let
# linuxdeploy pull in their libraries.
QP=/usr/lib/x86_64-linux-gnu/qt6/plugins
for d in wayland-shell-integration wayland-decoration-client; do mkdir -p $A/usr/plugins/$d; cp $QP/$d/*.so $A/usr/plugins/$d/; done
mkdir -p $A/usr/plugins/wayland-graphics-integration-client
cp $QP/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so $A/usr/plugins/wayland-graphics-integration-client/
WL_DEPS=()
for f in $A/usr/plugins/wayland-*/*.so; do WL_DEPS+=(--deploy-deps-only "$f"); done
/work/dl/linuxdeploy-x86_64.AppImage --appdir $A "${WL_DEPS[@]}"
# Copyright files of every bundled distro library
for l in $A/usr/lib/*.so*; do
  p=$(dpkg -S "$(basename "$l")" 2>/dev/null | head -1 | cut -d: -f1) || true
  [ -n "$p" ] && [ -f /usr/share/doc/$p/copyright ] && mkdir -p $A/usr/share/doc/$p && cp /usr/share/doc/$p/copyright $A/usr/share/doc/$p/
done
/work/dl/linuxdeploy-x86_64.AppImage --appdir $A --output appimage
mv misterMPV-*.AppImage /work/out/
echo APP-OK
