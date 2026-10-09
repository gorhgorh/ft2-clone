# PLAN: fastXtract — XM → stems / samples / MIDI / metadata via ft2-clone engine

## Status: implemented 2026-10-09 (see EXTRACT.md)

## Context

Goal = tracks, not software. 90s XM modules (e.g. `mods/BASTARDB.XM`: "Bastard Boy", 10 ch, 55 patt, 113 instr, speed 3, BPM 205) must land in Bitwig as separate audio stems (per channel, per instrument, per channel×instrument since sounds share channels), plus raw samples, pattern data as MIDI, and metadata (tempo, order list, names). Accuracy matters: use ft2-clone's own replayer + mixer, headless, via `--extract` flag in the existing binary (decision Q4). Format: 48 kHz / 32-bit float stereo stems (Q1). Both instrument and channel×instrument stems (Q2). MIDI in phase 1 (Q3).

Copy this plan to `.planning/PLAN-XM-EXTRACT.md` at implementation start (plan mode only allows this file).

## Key findings (exploration)

- No headless mode exists. `main()` (`src/ft2_main.c:53`) always inits SDL video + window + audio device.
- WAV renderer `src/ft2_wav_renderer.c` has reusable core: `dump_TickReplayer()` (:298, non-static) + loop :528-592 + `dump_EndOfTune()` (:284, static, 8 lines). GUI-bound otherwise.
- Mixer `src/ft2_audio.c`: `doChannelMixing()` (:499) loops `voice[i]` + fadeout `voice[32+i]`, each mix func accumulates into `audio.fMixBufferL/R` independently. `sendSamples32BitFloatStereo()` (:420) normalizes by `fAudioNormalizeMul` + clears buffers. Mix buffers only allocated in static `setupAudioBuffers()` (:927). `voice_t` (`ft2_audio.h:59`) has no instrument identity; fadeout voice is a struct copy (`*f = *v`, :227) so new fields inherit.
- `channel_t.instrPtr` latched in `triggerNote()` (`ft2_replayer.c:562`) = instrument actually triggering the sample. `ch->instrNum` unreliable (changes on instr-only rows w/o retrigger).
- Loader: `loadXM(FILE*, size)` (`modloaders/ft2_load_xm.c:28`) pure stdio, errors via `loaderMsgBox` fn ptr (`ft2_sysreqs.c:22`). `doLoadMusic()` (`ft2_module_loader.c:173`), `clearTmpModule()` (:228), `setupLoadedModule()` (:379) all static; core (copy tmp → live, sanitize, `fixSample`, clamps, `resetChannels`, `setSongPos`, `setMixerBPM`, `setLinearPeriods`) is :381-446 + :449-475 + :480-482 + :490. Rest is GUI.
- `lockAudio`/`pauseAudio` no-ops when `audio.dev == 0`. `setLinearPeriods` UI calls guarded by `ui.*` flags (zero headless). `calcMiscReplayerVars()` already called from `initializeVars()` path (`ft2_main.c:283`).
- Config: don't call `loadConfigOrSetDefaults()` (palette/mouse GUI). Set fields directly then `audioSetInterpolationType()`, `audioSetVolRamp()`, `setAudioAmp()` (`ft2_config.c:183-185`).
- Sample WAV writer `saveWAVSample()` (`ft2_sample_saver.c:321`) static, reads `editor.curInstr`. Reimplement (~120 lines): mono PCM, rate `getSampleC4Hz()`, `smpl` loop chunk, `LIST/INFO/INAM`. Must `unfixSample()` before / `fixSample()` after reading data (pattern in `saveXM`, `ft2_module_saver.c:257-263`).
- Pattern cell = `pattern[p][row*32 + ch]` (`note_t {note, instr, vol, efx, efxData}`); note 1-96 (C-0..B-7, 49 = C-4), 97 = key off; vol 0x10-0x50 = set vol 0-64; efx 0-35 = 0-9A-Z. Instrument names in `song.instrName[1..128]`.
- Build: CMake globs `src/*.c` → new file auto-included. Homebrew SDL2 (no framework; `make-macos.sh` won't work, cmake will).

## Approach: one-pass voice-routed stem mixing

Render song ONCE. Per tick, mix each voice separately, route its output into stem buffer keyed by (channel, instrument-tag). Channel stems and instrument stems derived by float summation of channel×instrument stems → exact, `sum(stems) == mix`, no mute leakage, ~100× faster than mute-and-rerender.

## Steps

### 1. Mixer: per-voice instrument tag + stem mixing (`src/ft2_audio.h`, `src/ft2_audio.c`)

- Add `uint8_t instrTag` to `voice_t`. Set in `updateVoices()` at `CS_TRIGGER_VOICE`: index of `ch->instrPtr` in `instr[]` (walk 1..128, or add `uint8_t instrIdx` to `instr_t` filled at allocate; prefer pointer compare helper `instrPtrToNum()`). Fadeout voice copies it via `*f = *v`.
- Expose `bool allocateMixBuffers(void)` (rename/wrap static `setupAudioBuffers`).
- Add `void mixReplayerTickToStems(uint32_t samplesToMix, stemSink_t *sink)`: same voice loop as `doChannelMixing` but after each voice (and its fadeout voice) mix call: scale `fMixBufferL/R[0..n)` by `fAudioNormalizeMul`, accumulate into `sink->buf[sink->route(ch, instrTag)]` (interleaved float stereo), zero mix buffers. Route callback + buffers live in extract module. Also provide optional "full mix" accumulation in same pass.

### 2. Loader: headless load (`src/ft2_module_loader.c/.h`)

- Split `setupLoadedModule()` → `static void setupLoadedModuleCore(void)` (lines 381-446, 449-475, 480-482, 490; drop `lockMixerCallback` pair, scrollbar calls stay in GUI part) + existing GUI remainder calling core first.
- Factor `doLoadMusic()` body into `static bool loadModuleFromFile(FILE *f)` (detect + dispatch + `moduleLoaded` flags) reused by `doLoadMusic`.
- New public `bool loadModuleHeadless(const char *path)`: set `loaderMsgBox`/`loaderSysReq` to stderr printers, `clearTmpModule()`, fopen, `loadModuleFromFile`, `setupLoadedModuleCore()`. Errors to stderr, return false.

### 3. Extract module (new `src/ft2_extract.c`, `src/ft2_extract.h`)

CLI: `ft2-clone --extract <mod> <outdir> [--rate 48000] [--bits 32|16] [--interp sinc8|sinc16|cubic|linear|none] [--rows-per-beat N] [--max-seconds 1800] [--no-stems] [--no-samples] [--no-midi]`

`int extractMain(int argc, char **argv)`:

1. `SDL_Init(0)`, `setupWindowedSincTables()`.
2. config: `interpolation` (default `INTERPOLATION_SINC8` = app default), `masterVol=256`, `boostLevel=1`, `specialFlags = BITDEPTH_32` (vol ramp ON, mirror GUI default `PRECISE_BPM` state from `defConfigData`). Apply via `audioSetInterpolationType`, `audioSetVolRamp(true)`, `setAudioAmp(1, 256, true)`.
3. `allocateMixBuffers()`, `setupReplayer()`, `loadModuleHeadless(path)`.
4. mkdir outdir + subdirs: `stems/channels`, `stems/instruments`, `stems/chan-instr`, `samples`, `midi`.
5. **Usage scan**: walk `song.orders[0..songLength)` patterns, per channel track last instrument (note w/o instr → last), collect set of (ch, instr) pairs + per-instr/ch usage counts. Superset OK (silent stems deleted later).
6. **Render pass** (if stems): init like `dump_Init` (:192-221) minus GUI wait: `setSongPos(0,0,RESET_SONG_TICK)`, `playMode=PLAYMODE_SONG`, `songPlaying=true`, `resetChannels()`, `setNewAudioFreq(rate)`, `setAudioAmp(...)`, `stopVoices()`, `song.globalVolume=64`, `setMixerBPM(song.BPM)`, `resetPlaybackTime()`, `editor.wavReachedEndFlag=false`. Loop: local copy of `dump_EndOfTune(song.songLength-1)` + max-seconds cap → `dump_TickReplayer()` → tick sample count (`samplesPerTickInt/Frac` logic :543-550) → `mixReplayerTickToStems()`. Stem files written incrementally (one `FILE*` per (ch,instr) pair, chunked like TICKS_PER_RENDER_CHUNK). **Timeline capture**: at each tick where `song.tick == song.speed` (row start) record `{songPos, pattNum, row, sampleOffset, speed, BPM}` → `timeline.json` + drives MIDI timing + `durationSeconds`.
7. After pass: finalize WAV headers (44-byte, float, from `dump_Close` :247-264); derive `stems/channels/ch-NN.wav` and `stems/instruments/iNNN-name.wav` by summing chan-instr files (streamed float add); `stems/mix.wav` = sum all. Delete stems whose peak == 0. Call `stopPlaying()`.
8. **Samples**: for each `instr[i]`, each `smp[j]` with data: `unfixSample`, write `samples/iNNN-sSS-name.wav` (mono PCM 8/16, rate `getSampleC4Hz`, `smpl` loop fwd/pingpong, `INAM`), `fixSample`. Sanitize names for filesystem.
9. **MIDI** (`midi/ch-NN.mid`, `midi/iNNN-name.mid`, type 1, PPQ 960): note = XM note + 11 (C-4=49 → 60); velocity = vol column 0x10-0x50 → `round(v/64*127)`, else sample default volume; note off at next note / key-off (97) / `ECx` on same channel, else song end. Timing: absolute seconds from timeline → beats at derived tempo `24·BPM/(rowsPerBeat·speed)` (initial values); default rowsPerBeat = `24/speed` clamped to {4,8,16} (speed 3 → 8 → 205 BPM for test mod). Also emit Set Tempo meta events where row duration changes (speed/BPM/pattern delay) so DAWs that honor tempo maps stay aligned. Minimal hand-rolled SMF writer (variable-length ints, track chunks), no deps.
10. **meta.json**: file, name, numChannels, songLength, restartPos, initial speed/BPM, linearPeriods, derived tempo (ticks/s, ms/row, rowsPerBeat, bpm), orders[], patterns[{idx,rows}], instruments[{num,name,fadeout,autoVib,envelope summary,samples[{num,name,length,bits,loop,c4hz,finetune,relNote,vol,pan,file}]}], usage{perChannel instr list, pairs}, durationSeconds, stem file index. **patterns.txt**: FT2-style text dump per used pattern (note/instr/vol/efx columns, decoding per `ft2_pattern_draw.c`).
11. Cleanup: free, `SDL_Quit`, exit code.

### 4. Hook in `main()` (`src/ft2_main.c`)

After `initializeVars()`/`setupCrashHandler()` (:88-89) and before `SDL_Init(AUDIO|VIDEO)` (:137):

```c
if (argc >= 3 && strcmp(argv[1], "--extract") == 0)
    return extractMain(argc, argv);
```

### 5. Repo hygiene

- `.gitignore`: add `mods/` (14 MB XM, personal) and `build/`.
- Build: `cmake -B build && cmake --build build -j` (Homebrew SDL2). Binary at `release/other/ft2-clone`.
- Note: this dir is own git repo (`origin gorhgorh/ft2-clone` fork), not yet zik subtree.

## Files

| File | Change |
|------|--------|
| `src/ft2_extract.c` / `.h` | new: CLI, render pass, stem sink/routing, WAV/MIDI/JSON writers (~700 lines) |
| `src/ft2_audio.c` / `.h` | `voice_t.instrTag`, `allocateMixBuffers()`, `mixReplayerTickToStems()` |
| `src/ft2_module_loader.c` / `.h` | split core, `loadModuleHeadless()` |
| `src/ft2_main.c` | 2-line hook |
| `.gitignore` | `mods/`, `build/` |

Reuse: `dump_TickReplayer` (`ft2_wav_renderer.h` — add decl if missing), `setNewAudioFreq`, `setAudioAmp`, `setMixerBPM`, `resetChannels`, `setSongPos`, `stopVoices`, `stopPlaying`, `getSampleC4Hz`, `unfixSample`/`fixSample`, `sanitizeInstrument/Sample`, `wavHeader_t` layout.

## Verification

1. Build: `cmake -B build && cmake --build build -j`; GUI still launches normally (`./release/other/ft2-clone`).
2. `./release/other/ft2-clone --extract mods/BASTARDB.XM out/bastardb` → exit 0, dirs populated, `meta.json` sane (10 ch, speed 3, BPM 205, 57 orders).
3. Sum check (python/numpy, both present): `sum(stems/channels/*.wav) ≈ stems/mix.wav` and `sum(stems/instruments/*.wav) ≈ mix.wav` (max abs diff < 1e-5).
4. Reference check: GUI "Render to WAV" (48 kHz, 32-bit, amp 1) of same mod vs `mix.wav`: same length ±1 tick, near-identical samples (differences only from dither-free float path, expect ~0).
5. Listen spot-check: `stems/channels/ch-01.wav` in Bitwig / `afplay`; one instrument stem contains only that sound.
6. MIDI: import `midi/ch-01.mid` into Bitwig at derived tempo next to `ch-01.wav`; note onsets align with transients across whole song (checks tempo/timeline handling incl. any Fxx/EEx).
7. Samples: open `samples/*.wav` in Bitwig sampler; loop points present (`smpl` chunk; verify with `sox --i` / `ffprobe`).
8. Edge: module with `Bxx` backward loop → render stops at `--max-seconds`; module load error → stderr message, exit 1, no window.

## Decisions (user, 2026-10-09)

- Q5: MIDI = absolute-time placement at single derived tempo, tempo-map events also emitted. Default now; tempo-change handling options later (see TODO).
- Q6: plain names `ch-NN.wav` / `iNNN-name.wav`; all stem/sample/midi file naming goes through ONE `stemName()`-style helper in `ft2_extract.c` so scheme is a one-place change.
- Q7: raw engine gain (sum == mix). Normalize = later option.

### 6. TODO file (`.planning/TODO-XM-EXTRACT.md`)

Create at implementation start; track deferred items:

- Tempo changes: option to ignore Fxx speed/BPM changes that are "pattern-level" x2 / ½ (half/double speed used to play patterns faster, not real tempo change) vs honoring real tempo changes. Needs heuristic (speed ratio exactly 2 / ½ with BPM unchanged → ignore) + flag `--tempo-mode honor|ignore-speed-doublings|flat`.
- Peak-normalize option for stems (`--normalize`).
- Analyse module: if every channel plays exactly one instrument (and each instrument on one channel), skip redundant per-channel + per-instrument sets, output one set only (report in meta.json which case applies).
- Stem naming with dominant instrument (`ch-01 kick.wav`) as option.

## Unresolved questions

None blocking.
