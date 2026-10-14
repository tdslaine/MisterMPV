#ifndef MPV_MISTER_PAD_H
#define MPV_MISTER_PAD_H

struct input_ctx;

// Start listening to the MiSTer's gamepad (UDP 32101) and turn button presses into mpv
// commands (seek, pause, playlist, tracks, volume, status OSD). The reader runs as an mpv
// input source and ends with mpv. No-op if the input stream cannot be opened.
void mister_pad_start(struct input_ctx *ictx, const char *host);

#endif
