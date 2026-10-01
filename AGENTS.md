# AGENTS.md — Rew. Reborn (Game Boy / GBC emulator)

## What this repo is
Win32 Game Boy / Game Boy Color emulator written in a single C++ translation
unit plus Win32 resources.

## Files
- `Rew. Reborn.cpp` — the entire emulator (CPU, PPU, APU, Bus, MBC1/3/5,
  save states, waveOut audio, window/menu handling).
- `resource.h` — header-only `#define`s (icon/menu/dialog IDs, 10 actions x 4
  key-binding slots). No includes.
- `resource.rc` — icon, main menu, key-setting dialog, VERSIONINFO.
- `resource.res` — compiled resources produced from `resource.rc`.
- `resource.res.o` — compiled resources as a COFF object (linked into the exe).

## Build (Linux cross-compile)
```
sudo apt-get install -y g++-mingw-w64-x86-64
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

## C++ style notes
- Spanish user-facing strings.
- Use octal escapes (`\341`) for accented characters inside string literals —
  `\xE1f` is parsed as one hex escape (`\xE1f`) and overflows, so hex escapes
  are unsafe when the next character is a hex digit.
