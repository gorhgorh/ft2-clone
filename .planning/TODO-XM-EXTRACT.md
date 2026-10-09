# TODO — XM extract (`ft2-clone --extract`)

Deferred items from 2026-10-09 planning. See `PLAN-XM-EXTRACT.md`.

- [ ] Tempo changes: option to ignore Fxx speed/BPM changes that are pattern-level x2 / 1/2 (half/double speed to play patterns faster, not a real tempo change) vs honoring real tempo changes. Heuristic: speed ratio exactly 2 or 1/2 with BPM unchanged = ignore. Flag `--tempo-mode honor|ignore-speed-doublings|flat`.
- [ ] Peak-normalize option for stems (`--normalize`). Default stays raw engine gain so stems sum to mix.
- [ ] Analyse module: if every channel plays exactly one instrument (and each instrument lives on one channel), skip redundant per-channel + per-instrument sets, output one set only. Report which case in `meta.json`.
- [ ] Stem naming with dominant instrument (`ch-01 kick.wav`) as option. Naming goes through one helper in `src/ft2_extract.c`.
- [ ] MIDI tempo map mode (`--midi-tempo-map`): place notes on the row grid and emit Set Tempo events at speed/BPM/pattern-delay changes, for DAWs that import tempo maps. Current default is absolute time at one derived tempo.
- [ ] 16-bit stems are plain clamp+round, no dither (GUI render uses 1-bit triangular dither). Add dither if 16-bit matters.
- [ ] Read amp/interpolation from the user's FT2.CFG instead of FT2 defaults (`--amp 10`, sinc8).
- [ ] Output size: ~23 MB/min per float stereo stem. Options: per-stem trimming of leading/trailing silence with offset in meta.json, or mono stems when pan is centered.
- [ ] Tone-portamento notes (3xx, 5xx, vol Fx) are emitted as new MIDI notes; FT2 slides the pitch without retrigger.
- [x] Pattern-level export (`--order A-B`, `--pattern N`, `--tail`, `--carry-voices`) done 2026-10-09.
- [ ] Small GUI (`tools/fastxtract`, Electron) for preview + export by pattern / channel / instrument. See `PLAN-XM-EXTRACT-GUI.md`.
