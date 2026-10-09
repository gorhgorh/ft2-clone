#pragma once

/* Headless extraction: ft2-clone --extract <module> <outdir> [options]
** Renders per-channel / per-instrument / per-(channel x instrument) stems with the
** real replayer+mixer in a single pass, dumps samples as WAV, patterns as MIDI/text,
** and metadata as JSON. Never opens a window or an audio device.
*/
int extractMain(int argc, char **argv);
