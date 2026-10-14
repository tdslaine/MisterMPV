# misterMPV reference

Technical reference for the MiSTer parts of this mpv fork. Overview and quick start: [README.md](README.md).
Status, decisions and open points: [HANDOFF.md](HANDOFF.md).

## Output options

Video (`--vo=mister`), audio (`--ao=mister`). The first driver that initialises opens the shared connection,
so pass the same host/modeline to both (the UI does) or use `$MISTER_HOST`.

| Option | Values | Default | Meaning |
|---|---|---|---|
| `--vo-mister-host` / `--ao-mister-host` | IP | `$MISTER_HOST` | MiSTer address (UDP 32100, pad on 32101) |
| `--vo-mister-modeline` / `--ao-mister-modeline` | `auto` or `WxH[p\|i]Hz` | `auto` | e.g. `320x240p60`, `640x480i60`, `720x576i50`; list in `video/out/mister_modelines.inc` |
| `--vo-mister-near` / `--ao-mister-near` | 0-3 | 1 | NLC codec: 0 lossless, 1-3 near-lossless |
| `--vo-mister-field-order` | `tff`, `bff` | `tff` | interlaced modes: which field goes first |
| `--vo-mister-flicker` | `off`, `light`, `full` | `light` | vertical low-pass 1-6-1 / 1-2-1 against 1-line flicker (interlaced only) |
| `--vo-mister-pad` | `yes`, `no` | `yes` | MiSTer gamepad controls playback |
| `--vo-mister-color-order` | `bgr`, `rgb` | `bgr` | channel order on the wire (the core expects B,G,R) |

`auto` starts at 640x480i60 and switches after ~8 frames: 25/50 fps content -> `320x288p50` / `720x576i50`,
everything else -> `320x240p60` (height <= 240) or `640x480i60`.

## Picture options (live, also while paused)

Set with `--name=value` or at runtime over IPC (`set_property`). Applied in the sender thread on the field/frame
that goes out.

| Option | Range | Meaning |
|---|---|---|
| `brightness`, `contrast`, `saturation` | -100..100 | mpv's own equalizer properties |
| `--mister-blacklevel` | 0-30 | lift blacks |
| `--mister-scanlines` | 0-100 | strength of the scanline pattern |
| `--mister-scanstyle` | 0-3 | 0 every 2nd row, 1 every 3rd, 2 dark + boosted, 3 RGB mask (at 240p try 1 or 2) |
| `--mister-hsize`, `--mister-vsize` | 80-100 | picture size in % of the raster (CRT overscan) |
| `--mister-hoffset`, `--mister-voffset` | -72..72 / -28..28 | shift in pixels / field lines |
| `--mister-calibrate` | yes/no | show the calibration test pattern instead of the video |

`--force-window` defaults to `yes` in this fork so the video output (and the gamepad / overlay) also exists
for audio-only files.

## Scripts (built in, `player/lua/`)

| Script option / message | Effect |
|---|---|
| `--script-opts=mister-shuffle=yes\|no`, `script-message mister-shuffle yes\|no` | Winamp-style shuffle: the list keeps its order, only the next entry is random |
| `script-message mister-next`, `mister-prev` | next/previous (random history when shuffling) |
| `--script-opts=mister-vis-mode=<m>`, `script-message mister-vis <m>` | visualizer for audio-only files: `off`, `spectrum`, `waves`, `vectorscope`, `spectrogram`, `bars` |
| `--script-opts=mister-track=yes\|no,mister-track-pos=top\|bottom`, `script-message mister-track yes\|no [pos]` | permanent artist/title overlay for music (size = `osd-font-size`) |

## Gamepad

The Groovy core streams the MiSTer controller state over UDP 32101; the layout is generic (the MiSTer maps any
pad to it): d-pad on bits 0-3, B1.. on `0x10`.. Mapping in `video/out/mister_pad.c`: D-pad L/R seek 10 s
(hold >400 ms = scan 20 s every 300 ms), U/D volume, B1 + B8 pause, B2 quit, B3 next audio, B4 next subtitle,
B5/B6 previous/next, B7 status text on the TV, B9/B10 seek -/+60 s. A button without a function flashes
`Pad button 0x....` on the TV.

## The control window (`mister-ui/`)

Qt6, talks to mpv over the JSON IPC socket (`--input-ipc-server`). Settings live in QSettings
(`~/.config/mistercast/mpv-mister-ui.conf`), tag cache in `~/.cache/misterMPV/misterMPV-tags2.json`.

- *Output*: host, codec, modeline, format (aspect), field order, flicker filter, **Test connection** (UDP status
  request, expects a 13-byte reply), **Launch core** (SSH: `load_core <newest matching .rbf>` into
  `/dev/MiSTer_cmd`; GroovyNLC or the original Groovy core), **Setup…**.
- *Setup…*: OSD text size, subtitle colour, subtitles in the black bars, artist/title overlay, colour order,
  gamepad on/off, SSH login (default root/1, stored unencrypted) and core file patterns.
- Command line: `misterMPV [files|folders|URLs...]` adds and plays. smb:// and other KIO locations are mapped to
  their kio-fuse paths (`$XDG_RUNTIME_DIR/kio-fuse-*/...`).

## Build notes

See README. The zig library is built by `meson.build` with `zig build --build-file ... -Doptimize=ReleaseFast`
when `third_party/groovy-mister-zig/zig-out/lib/libgroovy-mister-zig.a` is missing. `-Dmister-gmz-dir=` points
elsewhere. License: mpv GPL/LGPL, `nlc_codec` GPL.
