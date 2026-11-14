# AppImage build

Builds `misterMPV-<version>-x86_64.AppImage` in an Ubuntu 22.04 container (glibc 2.35), so it runs on most current
distributions. It bundles mpv (this fork), the `misterMPV` window, `ffprobe`, FFmpeg 7.1.2, libplacebo v7.351.0, Qt 6
(xcb and Wayland platform plugins) and the other libraries they need. Graphics drivers, the PulseAudio/PipeWire
server, `kio-fuse`, `jellyfin-mpv-shim` and `yt-dlp` stay on the host.

## Inputs (in `<work>/dl/`)

| File | Source | Check |
|---|---|---|
| `ffmpeg-7.1.2.tar.xz` (+ `.asc`) | https://ffmpeg.org/releases/ | GPG signature, FFmpeg release key `FCF986EA15E6E293A5644F10B4322F04D67658D8` |
| `libplacebo/` | `git clone --depth 1 --branch v7.351.0 --recurse-submodules https://code.videolan.org/videolan/libplacebo.git` | tag |
| `zig-x86_64-linux-0.15.2.tar.xz` | https://ziglang.org/download/ | sha256 `02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239` |
| `linuxdeploy-x86_64.AppImage` | linuxdeploy release `1-alpha-20251107-1` | sha256 `c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d` |
| `linuxdeploy-plugin-qt-x86_64.AppImage` | linuxdeploy-plugin-qt release `1-alpha-20250213-1` | sha256 `15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724` |

## Steps

```sh
W=<work dir>; cp packaging/appimage/* $W/; mkdir -p $W/dl $W/out $W/home   # put the inputs into $W/dl
git archive HEAD | (mkdir -p $W/src && tar x -C $W/src)
cd $W && docker build -t mistermpv-appimage-build:22.04 .
docker run --rm --user $(id -u):$(id -g) -e HOME=/work/home -v $W:/work mistermpv-appimage-build:22.04 /work/build-deps.sh
docker run --rm --user $(id -u):$(id -g) -e HOME=/work/home -v $W:/work mistermpv-appimage-build:22.04 /work/build-app.sh
```

`build-deps.sh` builds FFmpeg (LGPL, no X11 grabbing) and libplacebo (no GPU backends) once; `build-app.sh` builds
groovy-mister-zig with zig 0.15.2, mpv and the window, then runs linuxdeploy. Licenses end up in
`usr/share/doc/` inside the AppImage (our own, FFmpeg, libplacebo, and the Debian copyright file of every bundled
Ubuntu library).

## Test

`test-in.sh <AppImage> <some.mp3> <scratch dir>` checks mpv playback, the `mister` VO, `ffprobe` and that the window
starts (offscreen). Tested for v0.2.1 on CachyOS, Fedora 45 and Debian 13 containers with a desktop base
(alsa, mesa EGL/GL, libglvnd OpenGL, xkbcommon, fontconfig, harfbuzz, xcb, dbus).
