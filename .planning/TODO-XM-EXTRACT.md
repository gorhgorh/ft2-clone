# TODO — XM extract (`ft2-clone --extract`)

Deferred items from 2026-10-09 planning. See `PLAN-XM-EXTRACT.md`.

- [ ] Tempo changes: option to ignore Fxx speed/BPM changes that are pattern-level x2 / 1/2 (half/double speed to play patterns faster, not a real tempo change) vs honoring real tempo changes. Heuristic: speed ratio exactly 2 or 1/2 with BPM unchanged = ignore. Flag `--tempo-mode honor|ignore-speed-doublings|flat`.
- [ ] Peak-normalize option for stems (`--normalize`). Default stays raw engine gain so stems sum to mix.
- [x] Duplicate channel/instrument stems: moot since 2026-10-12, instrument stems are the only default; channels, pairs and mix are opt-in via `--stems`.
- [ ] Stem naming with dominant instrument (`ch-01 kick.wav`) as option. Naming goes through one helper in `src/ft2_extract.c`.
- [ ] MIDI tempo map mode (`--midi-tempo-map`): place notes on the row grid and emit Set Tempo events at speed/BPM/pattern-delay changes, for DAWs that import tempo maps. Current default is absolute time at one derived tempo.
- [ ] 16-bit stems are plain clamp+round, no dither (GUI render uses 1-bit triangular dither). Add dither if 16-bit matters.
- [ ] Read amp/interpolation from the user's FT2.CFG instead of FT2 defaults (`--amp 10`, sinc8).
- [ ] Output size: ~23 MB/min per float stereo stem. Options: per-stem trimming of leading/trailing silence with offset in meta.json, or mono stems when pan is centered.
- [ ] Tone-portamento notes (3xx, 5xx, vol Fx) are emitted as new MIDI notes; FT2 slides the pitch without retrigger.
- [x] Pattern-level export (`--order A-B`, `--pattern N`, `--tail`, `--carry-voices`) done 2026-10-09.
- [x] Small GUI (`tools/fastxtract`, Electron) first version 2026-10-09.
- [ ] fastxtract: bundle ft2-clone in the packaged app (electron-builder extraResources), waveform in preview bar, cancel running export, remember last module for preview + export by pattern / channel / instrument. See `PLAN-XM-EXTRACT-GUI.md`.
- [x] Tempo drift 2026-10-12: engine copied FT2/SB16 whole samples per tick (BPM 170 played at 170.213, 0.36 s drift over 5 min). Extract now renders exact BPM (`PRECISE_BPM`), `--ft2-tempo` keeps the old timing. Exports older than this carry the drift (their tempo map compensates, 170.21).
- [x] 2026-10-12: all 12 KALWORK mods re-exported in `~/SynologyDrive/musik/mods/exports/` with exact tempo, bar fitting and instrument stems only. Bitwig confirmed the 17/16 bar in chikin.
- [x] Bar fitting 2026-10-12: dawproject writes time signature changes so each order starts on a bar (odd bar absorbs leftover rows). Pending: confirm Bitwig honours TimeSignatureAutomation on import; if not, fallback is a silent gap or ignoring odd lengths.
- [x] Tempo analysis 2026-10-12 (`tools/fastxtract/src/shared/tempoAnalysis.ts`): main tempo, real changes, speed doublings (grid keeps tempo), odd pattern lengths; proposes the dawproject choices in the app and `--analyse` in the CLI. Covers the old `--tempo-mode` idea above.
