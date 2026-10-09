# ASSESS: XM to Dirtywave M8 converter (2026-10-09)

Status: v1 pipeline done 2026-10-09 in `tools/xm2m8` (zik repo). Spike + convert + analyze commands, BASTARD BOY converts (219 phrases, 107 chains, 27 song rows, 82 instruments, round trip verified). Awaiting device test on M8 Model:02 fw 6.5.0. Companion to `PLAN-XM-EXTRACT*.md`.

## M8 target facts (m8-files / m8-file-parser crates, manual v6)

- Song: 256 rows x 8 tracks of chain refs. Chain: 16 slots of (phrase, transpose). Phrase: 16 steps of (note, vel 0-7F, instr, 3 FX). Tables 256 x 16 rows.
- Budgets: 255 phrases, 255 chains, 128 instruments, 32 grooves.
- Sampler instrument: ONE wav per instrument (no keymap). Play modes FWD/REV/FWDLOOP/REVLOOP/FWD_PP/REV_PP/OSC/OSC_REV/OSC_PP. START / LOOP START / LENGTH / SLICE are 8-bit (1/256 of sample). DETUNE fine, no coarse-tune field beyond transpose. Samples stream from SD, no size limit, wav 8/16/24/32-bit, path <= 128 chars.
- Mods: 4 slots (AHD, ADSR, DRUM, LFO, TRIG, TRACK) to AMP/PITCH/PAN/CUTOFF/...
- Timing: step = 1/16 note, default groove 06 tics/step. M8 tic at 6 tics = 2.5/BPM s = exactly one XM tick at same BPM. So XM speed N @ BPM B == M8 TPO B + groove `N` (one groove row). Tick-based XM effects keep their units.
- File format: reverse-engineered, undocumented, firmware-version dependent (v3, v4, v4.1, up to v6 in `m8-file-parser`). Libraries: `m8-file-parser` (Rust, fw 4-6, read+write+remapper), `m8-files` (Rust, v4/4.1 rewrite), `m8-js` (JS, stale, pre-v4). All say: load a real template file and patch, never build from zero (undocumented regions).
- No software emulator. m8-headless = real Teensy. Testing needs a device (or m8c + headless Teensy).
- No existing XM/MOD -> M8 converter found.

## Measured on BASTARDB.XM (10 ch, 57 orders, 55 patt, 6509 rows)

| Budget | Need | Limit | Verdict |
|---|---|---|---|
| Phrases (unique 16-row x channel blocks, empty dropped) | 214 (203 with transpose dedup) | 255 | fits, tight |
| Chains (16 slots each, packed along order list) | ~26/track x 8 = ~208 | 255 | fits, tight |
| Instruments used | 82, all single-sample | 128 | fits |
| Tracks | 10 channels | 8 | must merge 2 pairs; pairs (0,6) (2,6) (2,8) (3,9) never overlap, merge free |
| Song rows | 57 orders, packed 4 patt/chain = ~15 | 256 | fits |
| Effects used | 2xx porta down 274, Fxx 18, Cxx 1, 1xx 1; vol column = set vol 97% | - | easy song |
| Samples | 72 x 16-bit, 22 x 8-bit, 1 x 32-bit | - | convert 8-bit to 16 |

Bigger 90s modules (16-32 ch, long patterns, heavy effects) WILL overflow phrases/chains: converter must report budgets and offer order-range export (same `--order A-B` as the GUI plan).

## Mapping (XM -> M8)

| XM | M8 | Loss |
|---|---|---|
| speed / BPM / Fxx | TPO + GRV (groove row = speed) | none |
| pattern delay EEx | extra groove row or duplicated step | ~none |
| pattern (N rows) | N/16 phrases, chained; N not multiple of 16 -> HOP / pad | none |
| order list, restart pos | song rows (chain refs), M8 loops whole song by default | restart pos != 0 lost |
| note 1-96 (C-0..B-7), key off 97 | note (M8 C-1 = 0?), OFF/KIL | verify base offset on device |
| instrument + default vol/pan | sampler instr, AMP/VOL + PAN | ok |
| sample fine tune / rel note | wav written at C-4 Hz (already done) or DETUNE + transpose | verify M8 pitch of non-44.1k wavs |
| loop forward | FWDLOOP + LOOP START (8-bit!) | loop point snaps to 1/256 of length: trim wav to [0,loopEnd] and pad start so loopStart lands on /256 grid, or pre-render loop region |
| loop pingpong | unroll to forward (region + reversed copy) | file 2x |
| vol env 12 pts + sustain + fadeout | ADSR/AHD fit to AMP; fadeout -> release | approximate |
| pan env | LFO/env to PAN | approximate |
| autovibrato | LFO to PITCH | approximate |
| vol column set vol | VEL | none |
| vol column slides, Axx | per-step VOL samples (one value per 16th) | tick-level ramps coarsened |
| 0xy arpeggio | ARP XY | none |
| 3xx tone porta | PSL + target note | ok |
| 1xx/2xx pitch slide | PBN (units differ, calibrate) | approximate |
| 4xx vibrato | PVB | ok |
| 7xx tremolo | VOL LFO | approximate |
| 9xx sample offset | STA (8-bit, scaled) | ok |
| E9x/Rxx retrig | RET | ok |
| ECx note cut, EDx delay | KIL, DEL | none |
| Bxx/Dxx jump/break | unroll order list at export | none |
| E6x pattern loop | unroll | phrase budget cost |
| Gxx/Hxx global vol | mixer / per-track VOL | approximate |
| Txx tremor, Lxx env pos, X1/X2 extra-fine, Kxx | drop (log) | rare in practice |

## Architecture

1. `ft2-clone --extract` adds `patterns.json` (cells per order/row/channel + per-row speed/BPM from timeline) and keeps wav export. ~100 lines C. Already have `meta.json` (instr/env/sample), `timeline.json`.
2. `tools/xm2m8` converter, reads extract dir, writes `song.m8s` + `Samples/<song>/*.wav`.
   - Writer: patch a blank `.m8s` saved from the user's firmware (template). Rust with `m8-file-parser` = lowest risk (fw 4-6, writer + remapper exist). TS port = zik stack but must copy offset tables and chase firmware changes. Lean: Rust CLI (soundfuzz precedent), GUI later calls it.
   - Steps: budget analysis + report -> channel merge (overlap matrix, greedy) -> phrase dedup (content hash, transpose-aware) -> chain packing -> instrument/sample prep (8->16 bit, loop alignment, pingpong unroll) -> envelope fit -> FX mapping -> write -> re-read with parser as self-check.
3. Device test loop: copy to SD, play, compare against stems from `--extract`.

## Effort (days, part-time)

| Piece | Days |
|---|---|
| patterns.json export (C) | 0.5 |
| budget analyzer + merge/dedup/packing | 1.5 |
| .m8s writer via template + crate, round-trip test | 2-3 |
| instrument/sample prep (loops, bits, pitch) | 1.5 |
| FX mapping core (ARP PSL PBN PVB RET KIL DEL STA VEL VOL PAN TPO GRV) | 2 |
| envelope -> ADSR/AHD fit | 1 |
| device iterations (pitch base, loop points, PBN units) | 2+, needs M8 |
| **Total "plays recognizably"** | **~11-12** |

Not 1:1. Tick-accurate volume ramps and multi-effect per tick would need M8 tables (TBL/TIC) per step: open-ended, skip in v1.

## Risks

- File format: no spec, firmware dependent, wrong bytes may crash M8 or load silently wrong. Mitigation: template + crate + re-read check, test early on device.
- 8-bit loop points: short chip loops will click/detune without wav pre-processing.
- Phrase/chain budget on big modules: hard wall. Order-range export is the escape hatch.
- Pitch base of sampler for arbitrary wav sample rates: unknown, test first.

## Decisions (2026-10-09)

- Q1: user owns M8 Model:02, recent firmware (likely 6.x). Exact version to confirm (Settings screen, or header of any `.m8s` on the SD). Save one blank song from the device as the template.
- Q2: as faithful as possible, differences accepted. So: invest in FX mapping + envelope fit + loop alignment; skip tables (TBL/TIC) emulation in v1.
- Q3: Rust + `m8-file-parser` crate. Reason: writer + remapper exist for fw 4-6, format drift tracked upstream; TS port would own every offset table per firmware. Binary format is the main risk, so lean on the crate. CLI binary, Electron GUI spawns it later like `ft2-clone --extract`. Fallback if crate lacks fw 6 write: fork it, still Rust.
- Q4: both policies, flag `--channels merge|drop|auto`. `auto` = merge zero-overlap pairs first, then drop least busy; always print the overlap matrix + what was merged/dropped.

## Done (2026-10-09)

- fw confirmed 6.5.0; parser fixture `EMPTY65.m8s` is a 6.5.0 blank song, bundled as template.
- `m8-file-parser` (git 08d68bc) writes 6.x songs; round trip verified.
- `patterns.json` already exported by `--extract` (GUI session added it), no C work needed.
- Rust crate `tools/xm2m8`: extract loader, channel plan (auto/merge/drop), FX mapping, sample prep (16-bit, loop trim, pingpong unroll), phrase/chain dedup + packing, grooves from speed, writer + re-read check, report.json. See its README for the mapping table and device checklist.

## Next

- Device test: pitch base / note offset, velocity scale, KIL as key off, HOP placement, PBN scaling, loop clicks, low sample rates.
- C export: envelope points, auto-vibrato, sample keymap (then envelope fit + multi-sample split in xm2m8).
- Transpose-aware phrase dedup; order-range export for big modules.
