# mpv + MiSTer (GroovyNLC)

Fork of mpv v0.41.0 with `--vo=mister` and `--ao=mister`: video is scaled/letterboxed to a CRT
modeline, GroovyNLC-encoded (YCoCg-R + Golomb-Rice) and streamed to a MiSTer running the
GroovyNLC core; audio goes as 48 kHz S16 stereo to the FPGA. Needs the core OSD options
"Volatile framebuffer = Off", "CRT scale = Off".

    mpv --vo=mister --ao=mister --vo-mister-host=<MiSTer IP> \
        --vo-mister-modeline=640x480i60 --vo-mister-near=1 file.mkv

Host can also come from `MISTER_HOST`. Modelines: see `video/out/mister_modelines.inc`
(`WxH[p|i]Hz`, e.g. `320x240p60`, `640x480i60`, `720x576i50`). `--ao-mister-*` options mirror the
VO ones; whichever driver initialises first decides, so put them in mpv.conf or use MISTER_HOST.

Build (meson 1.3+, groovy-mister-zig built at ../MiSTerCast/groovy-mister-zig, or `-Dmister-gmz-dir=`):

    meson setup build -Dlua=luajit -Dlibmpv=false -Dmanpage-build=disabled -Dtests=false
    ninja -C build

License: mpv GPL; nlc_codec is GPL (verbst/MiSTerCast).
