# Rew. Reborn

Emulador de **Game Boy** y **Game Boy Color** para Windows de 64 bits (Win32), en
un único `.exe` portable.

## Descargar y jugar

1. Descarga `rew_reborn.exe` de la [última versión](https://github.com/Dragonfire51/Rew.-Reborn/releases/latest).
2. Ábrelo y carga tu ROM con **File > Load Rom** (`.gb` y `.gbc`; también
   reproduce música de `.gbs`).
3. Teclas por defecto: flechas = cruceta, **Z** = A, **X** = B, **Espacio** =
   Select, **Enter** = Start y **Tab** mantenido = avance rápido. Se cambian en
   **Option > Key**.

No necesita instalación ni DLLs: el ejecutable lleva todo dentro.

## Compatibilidad con juegos comerciales

**Resumen: de cada 20 juegos comerciales, unos 19 arrancan y unos 19 se pueden
jugar de principio a fin.** El que falla casi siempre es un cartucho especial
(sensor de inclinación, cámara, infrarrojos) o un juego con trucos gráficos muy
concretos.

| Nivel | Qué significa para ti | Estimación |
|---|---|---|
| **Arranca** | El juego llega al menú o empieza a mostrarse | ~99 % |
| **Jugable** | Se puede jugar entero, con gráficos y sonido correctos | **~95 %** |
| **Sin fallos** | Además, sin glitches gráficos ni sonoros | ~87 % |
| **Idéntico a la consola** | Supera todas las pruebas estrictas de hardware | ~75–80 % |

Son cifras **estimadas** (v1.1.3) para los ~1.500 juegos licenciados de GB/GBC, no
un censo juego por juego (ver *Cómo se calculan*, más abajo).

### Lo que funciona

- **Cartuchos normales**, los que usan casi todos los juegos: MBC1, MBC2, MBC3 y
  MBC5, incluidos los multicart y los de hasta 64 Mb.
- **Juegos con reloj** (MBC3 + RTC, tipo Pokémon Oro/Plata/Cristal): el reloj va
  a la hora real, incluso con el emulador en pausa.
- **Partidas guardadas**: la batería se guarda sola en `gbsave\<rom>.sav` y los
  save states en `gbsave\<rom>.st0..st9` (10 ranuras).
- **Game Boy Color**: paletas, doble velocidad y bancos de VRAM/WRAM.
- **Super Game Boy**: bordes reales del juego y colorización con las paletas que
  envía el cartucho.
- **Cable link** entre dos ventanas del emulador (2 jugadores).
- **Avance rápido, 12 filtros de vídeo, remapeo de teclas, Auto A / Auto B**.

### Lo que no funciona (o funciona a medias)

| Juego o hardware | Estado |
|---|---|
| *Kirby Tilt 'n' Tumble*, *Command Master* (cartucho con sensor de inclinación, MBC7) | No arrancan |
| *Net de Get* (MBC6), *Robopon* (HuC3), *Tamagotchi 3* (TAMA5), juegos con MMM01 | No arrancan |
| *Game Boy Camera* | No funciona (la cámara no está emulada) |
| *Pokémon Oro/Plata/Cristal*: **Mystery Gift** (infrarrojos) | No disponible; el resto del juego sí |
| *Pokémon Pinball* y otros juegos con vibración | Sin vibración; el juego funciona |
| Juegos con sonido Super Game Boy | Sin sonido SGB; el color y el borde sí funcionan |
| Juegos de 4 jugadores | Solo 2 jugadores por cable link |
| Juegos que cambian el scroll a mitad de línea (muy pocos) | Pueden mostrar algún glitch gráfico menor |
| Cartuchos sin licencia (Sachen, Wisdom Tree, etc.) | Normalmente no arrancan |

### Cómo se calculan

Los porcentajes no salen de probar 1.500 juegos uno a uno, sino de dos cosas:

1. **Pruebas públicas de hardware**, que comprueban si el emulador se comporta
   como una consola real: instrucciones de la CPU y sus tiempos, temporizadores,
   DMA, sonido (12/12 pruebas en DMG y 12/12 en CGB), gráficos (`dmg-acid2` y
   `cgb-acid2` dan la imagen exacta, píxel a píxel) y mappers de cartucho (28/28).
2. **Qué hardware falta**: los porcentajes bajan por los cartuchos especiales de
   la tabla anterior, no por los juegos normales.

En las pruebas de hardware se supera el 92 % (lo que falla son pruebas del
arranque interno de la consola, que no afecta a los juegos) y el 100 % de los
mappers. Las suites son públicas y se pueden repetir:
[mooneye-test-suite](https://github.com/Gekkio/mooneye-test-suite),
[gb-test-roms](https://github.com/retrio/gb-test-roms) y
[acid2](https://github.com/mattcurrie/dmg-acid2).

## Compilar

Con MinGW-w64 (Linux):

```
x86_64-w64-mingw32-windres resource.rc resource.res.o
x86_64-w64-mingw32-g++ -std=c++17 "Rew. Reborn.cpp" resource.res.o \
    -o rew_reborn.exe -mwindows -static -static-libgcc -static-libstdc++ \
    -lwinmm -lcomdlg32 -lcomctl32 -lgdi32 -luser32
```

## Créditos

- Filtros de vídeo basados en los escaladores clásicos de Kreed / Andrea
  Mazzoleni, los mismos que usa VBA-M.
