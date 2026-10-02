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

## Windows 9x/ME port (branch `windows-9x-32bit`)
`main` builds a **PE32+ (64-bit)** exe, which does not load on Windows
95/98/98SE/ME. This branch adds a **32-bit (i686, PE32)** port:
```
sudo apt-get install -y g++-mingw-w64-i686 binutils-mingw-w64-i686
make -f Makefile.win9x          # -> rew_reborn_32.exe (PE32/i386)
make -f Makefile.win9x test     # core tests (host build, REW_HEADLESS)
```
- `-DREW_WIN9X` sets `_WIN32_WINNT 0x0400` and maps `SetWindowLongPtrA` /
  `GetWindowLongPtrA` to `SetWindowLongA` / `GetWindowLongA`. Windows 9x
  USER32 exposes only the `*LongA` variants (`*Ptr` is NT-only); on 32-bit
  `LONG_PTR == LONG`, so the mapping is safe. The 64-bit path is unchanged.
- **Post-9x imports**: the stock MinGW-w64 32-bit binary imported
  `KERNEL32!GetThreadId` (Vista+), which aborts startup on 9x with
  "el archivo ... está vinculado a una exportación de KERNEL32.DLL:GetThreadId
  que no existe". Root cause: the libstdc++ exception runtime pulls libgcc's
  `gthr-win32.o`, and `std::locale`/`std::string` pull the Vista+ condition
  variables. Fixes (all guarded so 64-bit is untouched):
  - no `<fstream>`/`<iostream>`: use `FILE*` (`FOut`/`FIn`/`LeerFichero`),
    which avoids `std::locale` and its `__gthr_win32_cond_*` imports.
  - build with `-fno-exceptions -fno-threadsafe-statics` (no local `static`
    guards, no exception tables) and the `*-win32` compiler variant (the
    `*-posix` one leaves `TryEnterCriticalSection`, NT4+).
  - provide `operator new`/`delete` and `__gthr_win32_mutex_*` shims so
    `eh_alloc.o`/`gthr-win32.o` are not linked (GetThreadId/TryEnterCriticalSection).
  Result: zero post-9x imports; KERNEL32 imports are all Win95/NT4-era.
- The binary must be 32-bit; a PE32+ image cannot run on 9x regardless of API.
- Real testing needs Win9x/ME hardware or a VM (86Box, PCem, VirtualBox/VMware).
- Runtime is cycle-accurate (M-cycle); may be tight on period hardware.

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
- v0.8.4 reflects Devin's latest fixes: Tomb Raider graphics/audio glitch
  fixes, the Pokemon Crystal WRAM-banking fix, new Extra functions and several
  bug fixes (documented in the About dialog).
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

## SGB borders (v0.8.1/v0.8.2)
- Real borders are decoded from the ROM: games send `CHR_TRN` ($13) and
  `PCT_TRN` ($14) packets over JOYP (`FF00`), and the emulator renders the
  result. `Bus::sgbWriteP1` receives the ICD2 bit protocol (start pulse =
  `P14=P15=0`; after that **P14 low (`0x10`) = bit 1, P15 low (`0x20`) = bit 0**,
  16 bytes LSB-first, each pulse followed by a return to `0x30`).
- The `_TRN` data is read from **VRAM `0x8800`** (i.e. `vram[0x800]`), *not*
  `0x8000`: `CHR_TRN` takes `vram[0x800..0x17FF]` (128 tiles x 32 bytes),
  `PCT_TRN` takes the tilemap from `vram[0x800..0xEFF]` and the palettes from
  `vram[0x1000..0x107F]`. Reading from `0x8000` yields an empty border.
- Border data format: 256 tiles x 32 bytes (4bpp, two interleaved GB tiles),
  a 32x28 tilemap, and four 16-color RGB555 palettes. Map entry: tile number =
  `e & 0xFF`, palette = `(e >> 10) & 3`, unused/transparent if `e & 0x300`.
  **Color index 0 of each palette is transparent** (this is what carves out the
  160x144 game window) — it is NOT bit 15 of the color.
- SGB detection: in SGB modes (`ModeIsSgb`) `reset()` sets `C=0x14` so games
  enable SGB features; `MLT_REQ` ($11) enables multiplayer and `readJoyp`
  returns the joypad ID (`$xE` = player 2, etc.) when both P14/P15 are high.
- CGB+SGB modes (`SYS_GBC_SGB`, `SYS_GBC_INIT_SGB`) run the game in **CGB mode**
  (`C=0x00`, full color) and do **not** let the game send its own border. Instead
  `CapturarBordeSgbParaCgb()` runs a throwaway copy of the cart as **DMG+SGB**
  (local `Bus`, `sgbSignatureMode=1`, up to `kSgbCaptureFrames`=600 frames until
  `sgbBorderFromPacket`), decodes the border into `g_sgb_captured_border`, and
  discards the copy. The real CGB session then displays that captured border.
  Forcing `C=0x14` in CGB mode is **not** viable: Gold/Silver/Crystal hang.
  This is triggered by `PrepararBordeParaModo()` on ROM load / mode switch.
- Border priority in `InitSgbBorder`: real ROM border (`sgbBorderFromPacket`)
  > captured border (`g_sgb_captured_border`) > external file > fallback style
  (only when `Force SGB Border` is on).
- `Force SGB Border` (`[System] ForceSgbBorder`) draws a themed border even for
  games that ship none; `Border Style` (`[System] BorderStyle`, -1 = auto)
  selects a `kSgbThemes` style (Pokemon Yellow/Gold/Silver/Red/Blue, Zelda...).
- External borders: `.<rom>.sgb`, `.\borders\<rom>.sgb` / `.bmp` or
  `.\border.sgb` / `.bmp`. `.sgb` = header "SGBR" + u32 w, u32 h, w*h u32 RGB.
- Save-state version is 9 (`kStateVersion`); SGB decoder state is serialized.

## Window title
- `g_window_base_title` is the base ("Rew. Reborn v0.8.4-alpha"); the loaded
  cartridge is shown as `"<base> - <ROM> - <FPS>"`. The ROM name comes from the
  header title (`0x134..0x143`, `EstablecerNombreRom`) or the filename.
- The title is refreshed both on ROM load/free (`ActualizarTituloVentana`) and
  once a second from the main-loop FPS counter.

## Speedup audio sync
- Speedup runs `SGB_SPEEDUP_MULT` frames per tick but keeps a stable real-time
  pace (`ticksPerEmuFrame`) so audio does not drift. `waveOutSetPlaybackRate`
  raises the device rate to `M x 44100` while held; if unsupported,
  `g_speedup_audio_muted` silences audio for the duration instead of
  desynchronising. `EnviarFrameAudio` drops queued chunks (`>2`) during speedup.
- **Playback-rate units**: `waveOutSetPlaybackRate` takes a *fixed-point
  multiplier*, not Hz — high word = integer part, low word = fraction
  (`0x10000` = 1.0, `0x40000` = 4.0, `0x8000` = 0.5). Always convert with
  `MultiplicadorTasaAudio(hz, hzBase)`. Passing raw Hz (44100 / 176400) is read
  as multipliers ~0.67x / 2.69x, which made normal-speed music play too low
  (v0.8.2 bug).

## C++ style notes
- Spanish user-facing strings.
- Use octal escapes (`\341`) for accented characters inside string literals —
  `\xE1f` is parsed as one hex escape (`\xE1f`) and overflows, so hex escapes
  are unsafe when the next character is a hex digit.
