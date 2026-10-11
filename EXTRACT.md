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
| `--order A[-B]` | whole song | render only order positions A..B (0-based), see below |
| `--pattern N` | | render pattern N alone from a clean state, like FT2 play-pattern |
| `--tail <s>` | 2 (0 for whole song) | ring-out after the range, last 100 ms faded |
| `--carry-voices` | off | `--order`: keep notes ringing from before A instead of cutting them |
| `--ft2-tempo` | off | FT2/SB16 tick rounding: BPM 170 really plays at 170.21, see below |
| `--no-stems` / `--no-samples` / `--no-midi` | | skip a family |
| `--all-samples` | | also write samples of instruments the song never plays |

## Output

```
out/song/
  meta.json          song, tempo, orders, instruments, samples, channel usage, stem index
  timeline.json      every pattern row the replayer read: order, pattern, row, frame, seconds, speed, bpm
  notes.json         the note events behind the MIDI files: channel (1-based), instr, MIDI key, vel, startFrame, endFrame
  patterns.txt       FT2-style text dump of every used pattern
  patterns.json      same as cells: [note, instr, vol, efx, efxData] raw XM values + FT2 display text, null when empty
  stems/mix.wav                       full mix, same as the GUI "Render to WAV"
  stems/channels/ch-NN.wav            one per channel
  stems/instruments/iNNN-name.wav     one per instrument, across all channels
  stems/chan-instr/chNN-iNNN-name.wav one per (channel, instrument) pair
  samples/iNNN-sSS-name.wav           raw samples, mono PCM, loop points in `smpl`, name in `INAM`; only instruments the song plays
  midi/ch-NN.mid                      notes of one channel
  midi/iNNN-name.mid                  notes of one instrument, one track per channel
```

All stems are stereo, full song length, same gain. They are mixed in the same pass, so channel stems, instrument stems and pair stems each sum exactly to `mix.wav`. Silent stems are deleted. Unused channels have no stem. Instruments the song never plays get no stem, no MIDI and no sample file (`--all-samples` keeps their samples); `meta.json` still lists them with `used: false`.

Stem size: 48 kHz float stereo is about 23 MB per minute per stem. A 4-minute song with 100 pairs is 10 GB; use `--stems channels,instruments,mix` or `--bits 16` when pairs are not needed.

## Ranges: one pattern or a few orders

```sh
./release/other/ft2-clone --extract mods/SONG.XM out/song --order 4-5   # -> out/song/orders/o04-o05/
./release/other/ft2-clone --extract mods/SONG.XM out/song --pattern 12  # -> out/song/patterns/p12/
```

Same layout as a full export (stems, samples, midi, meta.json, timeline.json), frame 0 = first row of the range.

`--order` plays the song silently from the start up to A, so speed, BPM, global volume and the instrument latched on each channel are the real in-song ones. Notes still ringing from before A are cut at frame 0 (`--carry-voices` keeps them; with it, `--order 4-5` is sample-identical to the matching slice of the full render). The range ends when the next order starts, when a jump leaves A..B, or on `F00`.

`--pattern` plays the pattern once from a clean state like FT2's play-pattern: initial speed/BPM, no latched instruments, so notes without an instrument column are silent, exactly as in FT2.

After the range, no more rows are read and the voices ring for `--tail` seconds (envelopes and fadeout keep running). `meta.json` has a `range` block (`rangeFrames`, `rangeSeconds`, `rows`, `tailSeconds`); `tempo` reflects the speed/BPM most rows of the range were played at. MIDI notes end at the range end, not in the tail.

## Tempo precision

A tick is exactly `2.5 / BPM` seconds, so a song at BPM 170 renders at 170.000 and sits on a DAW grid at 170 for its whole length. Real FT2 on a Sound Blaster 16 rounded the samples per tick to a whole number at 44 kHz (48 kHz in its WAV writer), which makes BPM 170 play at 170.213 and BPM 205 at 205.128: the GUI of this clone copies that by default ("more precise BPM" off), and `--ft2-tempo` does the same in the extract. The drift is 0.125 % at 170, about 0.36 s over a 5-minute song, so stems rendered that way only line up with a DAW tempo of 170.21. `meta.json` `tempo.ticksPerSecond` and `tempo.bpm` always give the real values, `tempo.ft2Tempo` says which mode was used.

## MIDI timing

Notes are placed by absolute time at one tempo: `24 * BPM / (rowsPerBeat * speed)` taken at the speed/BPM most rows use, so speed 3 / BPM 205 gives 205 BPM with 8 rows per beat (a one-row speed 1 intro does not decide the grid). Set the DAW project to the `tempo.bpm` value in `meta.json` and the MIDI lines up with the stems for the whole song, including after speed or BPM changes (there is no tempo map in the file). Velocity comes from the volume column, or the sample default volume. A note ends at the next note, key-off or `ECx` on the same channel, else at song end. MIDI note = XM note + 11 (C-4 is 60).

## Internals

- `src/ft2_extract.c`: CLI, render loop, stem sink, WAV/MIDI/JSON writers. All file naming goes through the `*Name()` helpers near the top.
- `src/ft2_audio.c`: `voice_t.instrTag` (set on trigger from the channel's latched instrument pointer), `mixReplayerTickToStems()` mixes each voice alone and hands it to a sink, `allocateMixBuffers()`.
- `src/ft2_module_loader.c`: `loadModuleHeadless()`, `setupLoadedModuleCore()` (the GUI-free half of module setup).
- `src/ft2_main.c`: two-line hook before SDL video init.
- `src/ft2_replayer.c`: `replayerPatternEnds` counter, bumped when a pattern ends (wrap, `Dxx`, `Bxx`); the range modes stop on it.

`tools/fastxtract` (Electron app) drives this CLI and can also build a `.dawproject` from an export dir (`bun run dawproject <exportDir>`), which Bitwig, Studio One and Cubase open directly.

Deferred ideas live in `.planning/TODO-XM-EXTRACT.md`.
