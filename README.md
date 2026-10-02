# Rew.-Reborn
Gameboy Color Emulator

## Rama `windows-9x-32bit` — port a Windows 9x/ME y Windows de 32 bits

Esta rama es un **port de 32 bits (i686)** pensado para funcionar en
**Windows 95 / 98 / 98SE / ME** y en cualquier Windows de 32 bits. No es la
rama principal (`main`); la versión de `main` es de 64 bits.

### Por qué una rama aparte
- `main` se compila como **PE32+ (64 bits)**, que **no arranca** en Windows 9x.
- En Windows 9x USER32 **no existe** `SetWindowLongPtrA`/`GetWindowLongPtrA`
  (son solo de NT). Este port usa `SetWindowLongA`/`GetWindowLongA`.
- El nivel de API se baja a `_WIN32_WINNT 0x0400` (Win95/NT4).

### Compilar
Toolchain de 32 bits (MinGW-w64 i686):

```
apt-get install g++-mingw-w64-i686 binutils-mingw-w64-i686
make -f Makefile.win9x          # genera rew_reborn_32.exe (PE32/i386)
```

El binario resultante (`rew_reborn_32.exe`) es PE32 (i386) y no importa
ninguna API posterior a Windows 9x.

### Verificar
```
make -f Makefile.win9x test     # pruebas del núcleo (CHR_TRN/PCT_TRN, tasa de audio, ...)
```

### Estado / limitaciones
- **Requiere 32 bits**: la clave es que sea PE32; un binario de 64 bits no
  funciona en 9x.
- Las pruebas de ejecución real en Windows 9x/ME deben hacerse en una máquina
  o VM (86Box, PCem, VirtualBox/VMware con imagen 9x).
- Rendimiento: el emulador es ciclo-a-ciclo (M-cycle); en hardware de la época
  puede ir justo.

