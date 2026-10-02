# AGENTS.md — Rew. Reborn (Game Boy / GBC emulator)

## What this repo is
Win32 Game Boy / Game Boy Color emulator written in a single C++ translation
unit plus Win32 resources.

## Files
- `Rew. Reborn.cpp` — the entire emulator (M-cycle CPU, PPU, APU, Bus,
  MBC1/2/3(+RTC)/5, CGB WRAM/VRAM banking, save states, waveOut audio,
  window/menu handling).
- `resource.h` — header-only `#define`s (icon/menu/dialog IDs, 10 actions x 4
  key-binding slots). No includes.
- `resource.rc` — icon, main menu, key-setting dialog, VERSIONINFO.
- `app_icon.ico` — 16x16 32bpp icon referenced by `resource.rc`.
- `resource.res` — compiled resources produced from `resource.rc`.
- `resource.res.o` — compiled resources as a COFF object (linked into the exe).

## Build (Linux cross-compile)
```
sudo apt-get install -y g++-mingw-w64-x86-64
x86_64-w64-mingw32-windres resource.rc resource.res.o
x86_64-w64-mingw32-g++ -std=c++17 "Rew. Reborn.cpp" resource.res.o \
    -o rew_reborn.exe -mwindows -static -static-libgcc -static-libstdc++ \
    -lwinmm -lcomdlg32 -lcomctl32 -lgdi32 -luser32
```
The `-static*` flags bundle the GCC/C++ runtime into the exe. Without them
the binary imports `libstdc++-6.dll` / `libgcc_s_seh-1.dll`, and running it
against an older MinGW runtime fails with "No se encuentra el punto de
entrada del procedimiento _ZSt28__throw_bad_array_new_lengthv".
Compiling just the source (`-c`) is a fast syntax check.

## Version / release conventions
- Version string: `vX.Y.Z-alpha`, used in three places in `Rew. Reborn.cpp`:
  the About dialog (`MostrarAcercaDe`), the `CreateWindowExA` window title, and
  the FPS `SetWindowTextA` update.
- `resource.rc` VERSIONINFO must match: `FILEVERSION`/`PRODUCTVERSION` as
  `X,Y,Z,0` and the `FileVersion`/`ProductVersion` string values.
- When the version or user-facing feature list changes, also update
  `resource.res.o` (the compiled resource object). The version lives in
  `VS_FIXEDFILEINFO` (signature `bd 04 ef fe`) and in UTF-16 `FileVersion` /
  `ProductVersion` strings; both can be patched in place (same byte length).
- Keep `resource.rc` line endings as CRLF.

## Features (v0.8)
- `Option > System` modes: Auto, Game Boy (DMG), Super Game Boy,
  Game Boy + SGB Border, Game Boy Color + SGB Border and
  Game Boy Color + Initial SGB Border. Stored in the ini as
  `[System] Model` (0..5). The SGB modes render a 256x224 border around the
  160x144 game viewport; the border is generated procedurally in
  `GenerarBordeSgb` (no large data tables) and composited by `ComponerFrameSgb`.
- Speedup Toggle: a mappable key (default Tab) that runs several frames per
  tick while held (`SGB_SPEEDUP_MULT`), skipping the real-time sync.
- Auto A / Auto B: mappable keys that toggle automatic fire on the A / B
  buttons (`g_auto_fire_enabled`), for all 4 key profiles.
- `MAX_ACTIONS` is 11 (UP, DOWN, LEFT, RIGHT, A, B, Select, Start, AutoA,
  AutoB, Speedup). Adding an action requires: a row in `g_bindings`,
  `g_action_names`, `kAllCtrlIds`, and matching `IDC_KEY_*` edit boxes plus
  `RTEXT` labels in `IDD_KEY_SETTING`.
- The Key Setting dialog captures Tab as a binding (no focus navigation) so
  Speedup can be mapped to Tab.

## SGB borders (v0.8.1)
- Real borders are decoded from the ROM: games send `CHR_TRN` ($13) and
  `PCT_TRN` ($14) packets over JOYP (`FF00`), and the emulator renders the
  result. `Bus::sgbWriteP1` receives the ICD2 bit protocol (start pulse =
  `P14=P15=0`, bit 0 = `P14` low, bit 1 = `P15` low, 16 bytes LSB-first).
- Border data format: 256 tiles x 32 bytes (4bpp, two interleaved GB tiles),
  a 32x28 tilemap, and three 16-color RGB555 palettes (SNES palettes 4-6).
  **Color index 0 of each palette is transparent** (this is what carves out the
  160x144 game window) — it is NOT bit 15 of the color.
- SGB detection: in SGB modes (`ModeIsSgb`) `reset()` sets `C=0x14` so games
  enable SGB features; `MLT_REQ` ($11) enables multiplayer and `readJoyp`
  returns the joypad ID (`$xE` = player 2, etc.) when both P14/P15 are high.
- Border priority in `InitSgbBorder`: real ROM border (`sgbBorderFromPacket`)
  > external file > fallback style (only when `Force SGB Border` is on).
- `Force SGB Border` (`[System] ForceSgbBorder`) draws a themed border even for
  games that ship none; `Border Style` (`[System] BorderStyle`, -1 = auto)
  selects a `kSgbThemes` style (Pokemon Yellow/Gold/Silver/Red/Blue, Zelda...).
- External borders: `.<rom>.sgb`, `.\borders\<rom>.sgb` / `.bmp` or
  `.\border.sgb` / `.bmp`. `.sgb` = header "SGBR" + u32 w, u32 h, w*h u32 RGB.
- Save-state version is 9 (`kStateVersion`); SGB decoder state is serialized.

## C++ style notes
- Spanish user-facing strings.
- Use octal escapes (`\341`) for accented characters inside string literals —
  `\xE1f` is parsed as one hex escape (`\xE1f`) and overflows, so hex escapes
  are unsafe when the next character is a hex digit.
