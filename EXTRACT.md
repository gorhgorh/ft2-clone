# `ft2-clone --extract` — module to DAW stems

Headless mode of this FT2 clone. Loads a module with the real loader, runs the real replayer and mixer once, and writes everything a DAW needs. Never opens a window or an audio device.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# binary: release/other/ft2-clone
```

Needs SDL2 (Homebrew `sdl2` on macOS is fine; the stock `make-macos.sh` expects the framework install).

## Usage

```sh
./release/other/ft2-clone --extract mods/SONG.XM out/song [options]
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--rate <hz>` | 48000 | stem sample rate |
| `--bits 32\|16` | 32 | 32 = float, 16 = PCM (no dither) |
| `--amp <1..32>` | 10 | FT2 "Amp." boost level, 10 is the FT2 default config |
| `--interp <mode>` | sinc8 | none, linear, quadratic, cubic, sinc8, sinc16 |
| `--stems <list>` | all | comma list of `channels`, `instruments`, `pairs`, `mix` |
| `--rows-per-beat <n>` | 24/speed | MIDI grid (speed 6 = 4, speed 3 = 8) |
| `--max-seconds <s>` | 1800 | safety cap for songs that loop forever |
| `--no-stems` / `--no-samples` / `--no-midi` | | skip a family |

## Output

```
out/song/
  meta.json          song, tempo, orders, instruments, samples, channel usage, stem index
  timeline.json      every pattern row the replayer read: order, pattern, row, frame, seconds, speed, bpm
  patterns.txt       FT2-style text dump of every used pattern
  stems/mix.wav                       full mix, same as the GUI "Render to WAV"
  stems/channels/ch-NN.wav            one per channel
  stems/instruments/iNNN-name.wav     one per instrument, across all channels
  stems/chan-instr/chNN-iNNN-name.wav one per (channel, instrument) pair
  samples/iNNN-sSS-name.wav           raw samples, mono PCM, loop points in `smpl`, name in `INAM`
  midi/ch-NN.mid                      notes of one channel
  midi/iNNN-name.mid                  notes of one instrument, one track per channel
```

All stems are stereo, full song length, same gain. They are mixed in the same pass, so channel stems, instrument stems and pair stems each sum exactly to `mix.wav`. Silent stems are deleted. Unused channels have no stem.

Stem size: 48 kHz float stereo is about 23 MB per minute per stem. A 4-minute song with 100 pairs is 10 GB; use `--stems channels,instruments,mix` or `--bits 16` when pairs are not needed.

## MIDI timing

Notes are placed by absolute time at one tempo: `24 * BPM / (rowsPerBeat * speed)`, so speed 3 / BPM 205 gives 205 BPM with 8 rows per beat. Set the DAW project to the `tempo.bpm` value in `meta.json` and the MIDI lines up with the stems for the whole song, including after speed or BPM changes (there is no tempo map in the file). Velocity comes from the volume column, or the sample default volume. A note ends at the next note, key-off or `ECx` on the same channel, else at song end. MIDI note = XM note + 11 (C-4 is 60).

## Internals

- `src/ft2_extract.c`: CLI, render loop, stem sink, WAV/MIDI/JSON writers. All file naming goes through the `*Name()` helpers near the top.
- `src/ft2_audio.c`: `voice_t.instrTag` (set on trigger from the channel's latched instrument pointer), `mixReplayerTickToStems()` mixes each voice alone and hands it to a sink, `allocateMixBuffers()`.
- `src/ft2_module_loader.c`: `loadModuleHeadless()`, `setupLoadedModuleCore()` (the GUI-free half of module setup).
- `src/ft2_main.c`: two-line hook before SDL video init.

Deferred ideas live in `.planning/TODO-XM-EXTRACT.md`.
