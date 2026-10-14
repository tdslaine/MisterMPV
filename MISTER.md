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

## Build (self-contained)

Everything needed is in this repo: the mpv sources, `video/out/nlc_codec.*` (NLC codec) and
`third_party/groovy-mister-zig` (MiSTer protocol library incl. its lz4 dependency in `zig-pkg/`).
meson builds the zig library itself on the first configure. Verified from a fresh `git clone`.

Tools: `meson` (>= 1.3), `ninja`, `zig` (>= 0.15.2, tested 0.16.0), `cmake` + Qt6 base (UI),
a C/C++ compiler, and mpv's usual dev packages (ffmpeg, libass, luajit, pipewire, ...; on Arch
`pacman -S mpv` pulls most). If the distro has no meson: `python3 -m venv v && v/bin/pip install meson`.

    meson setup build -Dlua=luajit -Dlibmpv=false -Dmanpage-build=disabled -Dtests=false
    ninja -C build                                   # -> build/mpv (single binary)
    cmake -S mister-ui -B mister-ui/build -G Ninja && ninja -C mister-ui/build
                                                     # -> mister-ui/build/misterMPV (UI)

After moving the folder: `meson setup --wipe build` and delete `mister-ui/build`.
Override the library location with `-Dmister-gmz-dir=<path>`.

License: mpv GPL; nlc_codec is GPL (verbst/MiSTerCast).

## UI

`mister-ui/` is a small Qt6 front-end (file/URL, host, modeline incl. `auto`, codec, play/pause,
seek, audio/subtitle track, loop). It starts mpv (`$MPV_MISTER`, else `mpv` next to the UI binary, else `../../build/mpv`) and controls
it over the JSON IPC socket. Binary name: `misterMPV`.

    mister-ui/build/misterMPV

`--vo-mister-modeline=auto` (default) switches the modeline after ~8 frames: 25/50 fps content ->
50 Hz (`320x288p50` / `720x576i50`), everything else -> 60 Hz (`320x240p60` for height <= 240,
else `640x480i60`).

See the HANDOFF of the MiSTerCast repo (section "misterMPV") for the full option list, gamepad mapping and status.
