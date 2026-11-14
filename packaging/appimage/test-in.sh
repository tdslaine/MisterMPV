#!/bin/sh
# runs the AppImage's three programs; $1 = AppImage, $2 = media file
set -u
A=$1; export APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME=${3:-/tmp}/c XDG_CACHE_HOME=${3:-/tmp}/k
W=${3:-/tmp}; cd $W && rm -rf squashfs-root && $A --appimage-extract >/dev/null && R=$W/squashfs-root
grep -m1 PRETTY_NAME /etc/os-release

$R/usr/bin/mpv --no-config --vo=null --ao=null --length=1 av://lavfi:testsrc >$W/m.log 2>&1 && echo "mpv playback: OK" || { echo "mpv playback: FAIL"; tail -3 $W/m.log; }
$R/usr/bin/mpv --no-config --vo=help 2>/dev/null | grep -q mister && echo "vo mister: OK" || echo "vo mister: FAIL"
$R/usr/bin/ffprobe -v error -show_entries format_tags=title -of csv=p=0 "$2" && echo "ffprobe: OK" || echo "ffprobe: FAIL"
timeout 5 $A >$W/ui.log 2>&1; rc=$?; [ $rc = 124 ] && echo "UI (AppImage, offscreen) running after 5 s: OK" || { echo "UI: FAIL rc=$rc"; tail -5 $W/ui.log; }
