# PLAN: pattern-level export + small GUI (draft, 2026-10-09)

Status: thinking, not started. User ask: small GUI, preview a pattern, export it as whole / per channel / per instrument.

## Step 1: CLI first (any GUI needs it)

- `--order A[-B]`: render order positions A..B with real song state (instruments latched, global vol, speed/BPM from earlier patterns). Exact, same as song playback.
- `--pattern N`: render pattern N alone from clean state (= FT2 "play pattern" mode, `PLAYMODE_PATT`). Clean but may differ from in-song sound when a note has no instrument column.
- Both reuse `renderSong` loop: start pos from `setSongPos(order, 0)`, stop condition = order > B or pattern wrapped. Add tail option `--tail <s>` so release/echo of last row is kept (default 2 s).
- Output layout: `out/song/patterns/pNN/` or `out/song/orders/oNN-oMM/` with same `stems/`, `midi/`, `meta.json` subset. Naming via existing helpers.
- Also list in `meta.json`: `orders[]` already there, patterns with row counts already there.

## Step 2: GUI options

| Option | Stack | Preview | Pros | Cons |
|--------|-------|---------|------|------|
| A. inside ft2-clone | C + SDL, 640x400 8-bit widgets | native, already plays patterns | one binary, exact | C GUI code painful, upstream merge friction, ugly to grow |
| B. web app `tools/ft2-extract-ui` | React + Vite + Tailwind, Bun server spawns binary | render pattern to WAV (fast, under 1 s), play via `<audio>`, waveform | zik stack, easy to grow (drag stems, batch, presets) | two processes, needs step 1 |
| C. TUI | Bun + Ink | `afplay` shellout | quick | poor preview, no waveform |

Lean: B. Pattern grid read from `patterns.txt`/`meta.json`, left column = orders list, click = preview, checkboxes = channels / instruments, export button = CLI call with `--order` / `--pattern` + `--stems`.

## Decisions (2026-10-09)

- Q2: both `--order A-B` and `--pattern N`.
- Q3: exports inside the song out dir (`out/song/patterns/pNN/`, `out/song/orders/oNN-oMM/`).
- Q1: user leans Electron direct instead of web. Rationale web was only "less setup"; for this tool Electron wins: double-click app, no terminal, spawns `ft2-clone --extract` via child_process, native folder dialogs, open in Finder, and native drag of stem files straight into Bitwig (`webContents.startDrag`), which a browser cannot do. Reuse `tools/ado` scaffold (electron-vite + React 19 + Tailwind 4 + @almst/ui + Jotai). Tauri 2 (as in soundfuzz) is lighter but needs Rust sidecar config for the binary; Bun-native shells (Electrobun) too immature. Confirmed by user: Electron, same stack and UI elements as the other zik Electron apps (`tools/ado`), reuse `@almst/ui` so it keeps the same look.

## Unresolved questions

- Q4: app name/dir, e.g. `tools/fastxtract`.
