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

## Features (v0.10)
- v0.10.0 adds a Link Cable (game link) between two emulator instances over
  shared memory. Each instance owns one 4-byte slot (its side of the cable) plus
  a lock byte in the file mapping `Local\RewRebornLink` (`LinkCable` in
  `Rew. Reborn.cpp`); the peer is the other slot. When a game starts an external
  serial transfer (`SC` = 0x81/0x83, or 0x80 as a slave while the link is on),
  the emulator publishes `SB` to its slot; when the transfer completes
  (`tickM` counts `serialCycles` down) it pulls the peer's byte if present,
  otherwise 0xFF. `Option > Link Cable` has: `Link Cable activado`
  (enable/disable, `g_link_enabled`, ini `[Link] Enabled`), `Escuchar (modo
  esclavo)` (sets role to slave), `Recibir byte del rival` (shows the pending
  peer byte), `Cambiar rol (maestro/esclavo)` (ini `[Link] Role`) and
  `Reiniciar cable`. With no cable the serial port behaves as before (0xFF).
  The `REW_HEADLESS` stub keeps the same two-slot model so the logic is
  testable without Windows.
- v0.9.1 adds a proper wall-clock RTC for MBC3 clock cartridges. The old code
  advanced `rtcRegs` from CPU cycles (`rtcTick(dots)` in `tickM`), so the clock
  froze while paused and ran 4x during Speedup. Now the registers hold civil
  time and `rtcSync()` resolves `rtcRegs + (now - rtcBase)` on every RTC access
  (`cartRamRead`/`cartRamWrite`/`rtcLatch`/`batteryData`), so the clock advances
  in real time regardless of pause or Speedup. `rtcSetFromWallClock()` seeds it
  from `time()`/`gmtime()` when a clock cart is loaded with no `.sav`;
  `loadBatteryData()` resumes from the saved time and applies the elapsed
  seconds. The halt (bit 6) and carry (bit 7) flags and the 0x00->0x01 latch
  command are honoured. Toggle with `Option > System > Real Time Clock (RTC)`
  (`g_rtc_enabled`, ini `[System] RealTimeClock`); `Poner RTC a la hora del
  sistema` reseeds it. RTC state is `mutable` so the const cart reads can sync;
  save-state version is 10.
- v0.9.0 adds a real-time `Option > Video > Filter` submenu with 12 filters:
  None, 2xSaI, Super 2xSaI, Super Eagle, Advance MAME Scale2x, Bilinear,
  Bilinear Plus, Scanlines, TV Mode, Simple 2X, Simple 3x and Simple 4x.
  The `Option > Video` popup now holds two submenus: `Size` (x1-x4, the old
  size items) and `Filter` (runtime-menu IDs `ID_FILTER_BASE + n`, built in
  `CrearMenusExtra`). The active filter is stored in the ini as `[Video]
  Filter` and applied in `AplicarFiltroVideo()` to the whole displayed frame
  (GB 160x144 or SGB 256x224, including the border), output to
  `g_displayBuffer` (up to 1024x896 for SGB 4x); `WM_PAINT` stretches that
  buffer to the window. `ResizeEmulatorWindow` accounts for the filter
  magnification (`kFilterMag`) times the Size scale. Filter implementations
  are the classic Kreed/Andrea Mazzoleni scalers (GPL-2.0, as used by VBA-M)
  adapted to 0x00RRGGBB 32-bit buffers.
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
- `Option > Audio` submenu (runtime menu IDs, `CrearMenusExtra`):
  `Sonido On`/`Sonido Off` (the master audio toggle, formerly `ID_AUDIO_TOGGLE`
  40050, kept for compatibility), `Reverb`, `Low Pass Filter Level 1/2`, and a
  `Volumen del audio` submenu with 0/25/50/100/150/200%. Effect options are
  mutually exclusive and clicking the active one clears it (`AUDIO_FX_NONE`).
  Stored in the ini as `[Audio] Enabled`, `[Audio] Effect` (0=none, 1=reverb,
  2=LPF1, 3=LPF2) and `[Audio] Volume` (percent).
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
- Save-state version is 10 (`kStateVersion`); SGB decoder state is serialized.

## Window title
- `g_window_base_title` is the base ("Rew. Reborn v0.10.0-alpha"); the loaded
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
