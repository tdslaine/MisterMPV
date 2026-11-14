# Credits and sources

misterMPV stands on a lot of other people's work. This file lists **what we took, from where, how we use it and under
which license**. If something here is wrong or missing, please open an issue.

## Code that is part of this repository

| Source | What we use | How | License |
|---|---|---|---|
| **[mpv](https://github.com/mpv-player/mpv)** (mpv-player) | the whole player, **v0.41.0** (`git remote origin`) | forked; our additions are new files plus a few hook lines (see README) | GPLv2+ (libmpv LGPLv2.1+) |
| **[groovy-mister-zig](https://github.com/dylan/groovy-mister-zig)** by **dylan** | UDP protocol library for Groovy (video/audio/input, frame pacing), vendored in `third_party/groovy-mister-zig/`; upstream base commit `2df38f5` | copied as plain files **with local changes** (NLC codec support, input, pacer fixes) | GPL-2.0 (`third_party/groovy-mister-zig/LICENSE`) |
| **[zig-lz4](https://github.com/jedisct1/zig-lz4)** by **jedisct1** (Frank Denis) | LZ4 binding, dependency of groovy-mister-zig, vendored as `third_party/groovy-mister-zig/zig-pkg/lz4-…` (commit `55ebd6e`) | unchanged copy, needed to build without network | MIT (`third_party/groovy-mister-zig/zig-pkg/lz4-…/LICENSE`) |
| **[verbst/Groovy_MiSTer](https://github.com/verbst/Groovy_MiSTer)** by **verbst** | `video/out/nlc_codec.cpp/.h` from `api/` (commit `e60f52a`) - the GroovyNLC encoder, must stay bit-exact with the FPGA decoder | copied, byte-identical apart from one added header line | GPL-2.0 |

## Cores and protocols we talk to (not part of this repo)

| Source | Role |
|---|---|
| **[psakhis/Groovy_MiSTer](https://github.com/psakhis/Groovy_MiSTer)** by **psakhis** | the **Groovy core** and the UDP streaming protocol everything here speaks (GPL-2.0) |
| **[verbst/Groovy_MiSTer](https://github.com/verbst/Groovy_MiSTer)** by **verbst** | the **GroovyNLC core** (near-lossless codec decoder in the FPGA) that misterMPV requires |
| **[MiSTer FPGA](https://github.com/MiSTer-devel/Main_MiSTer)** | the platform |

## Ideas and reference implementations (no code copied)

| Source | What we took |
|---|---|
| **[idio-sync/MiSTer_GroovyRelay](https://github.com/idio-sync/MiSTer_GroovyRelay)** | the **interlace flicker filter** (vertical 1-6-1 "light" / 1-2-1 "full" low-pass), the idea of **picture size/position calibration** for CRT overscan, and the `load_core` command written to `/dev/MiSTer_cmd` over SSH. Re-implemented in C/Qt; GroovyRelay is Go (GPL-3.0). |
| **[iequalshane/MiSTerCast](https://github.com/iequalshane/MiSTerCast)** (Shane Lynch et al.) | the original MiSTerCast client for Windows, which our Linux port (sibling project) grew out of. The timing values of the modeline presets in `video/out/mister_modelines.inc` are derived from its `FrontEnd/modelines.dat` (which in turn credits podernixie's mode tables); only numeric timings were taken, no code. |
| **[idio-sync/MiSTer_GroovyRelay](https://github.com/idio-sync/MiSTer_GroovyRelay)** | timing values for the 720x240p / 720x288p modeline presets |
| **Winamp** | the look & feel of the window (table playlist, transport row, sort menu). Winamp is a trademark of its owner; no affiliation, no code. |

## Libraries and tools used

| | |
|---|---|
| [FFmpeg](https://ffmpeg.org) | decoding in mpv; `ffprobe` for tags/durations |
| [libass](https://github.com/libass/libass), [LuaJIT](https://luajit.org) | subtitles / mpv scripting (our Lua scripts) |
| [Qt 6](https://www.qt.io) | the control window (LGPL) |
| [Zig](https://ziglang.org), [Meson](https://mesonbuild.com), [Ninja](https://ninja-build.org), [CMake](https://cmake.org) | build |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | YouTube and other stream URLs (via mpv's `ytdl_hook`) |
| [OpenSSH](https://www.openssh.com) | "Launch core" on the MiSTer |
| [jellyfin-mpv-shim](https://github.com/jellyfin/jellyfin-mpv-shim) (GPL-3.0) | the optional Jellyfin receiver: misterMPV starts a private instance as a separate program, which plays through our `mpv` |
| [Jellyfin](https://jellyfin.org) (GPL-2.0) | the media server the receiver connects to (Quick Connect, casting, SyncPlay); not bundled |
| [KDE kio-fuse](https://invent.kde.org/network/kio-fuse) | network shares (smb://, sftp://) as plain paths |
| [LZ4](https://lz4.org) (Yann Collet) | via zig-lz4 |

## Written by us

`vo_mister`, `ao_mister`, the shared sender thread with field splitting / flicker filter / picture effects,
CRT calibration and test pattern, the gamepad bridge, the Lua scripts (shuffle, visualizers, artist/title overlay),
the whole `mister-ui` window, and the build integration.

## Sibling project

**MiSTerCast for Linux** (screen / game / media cast to the same core, KDE/Wayland) - the project this player grew out
of; the CRT calibration, the flicker filter and the colour handling were first built there.
