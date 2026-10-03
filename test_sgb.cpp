// Prueba del decodificador de borde SGB (paquetes CHR_TRN/PCT_TRN por JOYP).
#include "Rew. Reborn.cpp"
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]" : "[FALLO]", what);
    if (!ok) g_fail++;
}

// Envia un paquete SGB completo (start + 16 bytes LSB-first + stop) por FF00.
static void sgbSendPacket(Bus& b, const uint8_t* p) {
    b.ioWrite(0xFF00, 0x30);
    b.ioWrite(0xFF00, 0x00);            // pulso de start
    for (int i = 0; i < 16; ++i)
        for (int bit = 0; bit < 8; ++bit) {
            b.ioWrite(0xFF00, ((p[i] >> bit) & 1) ? 0x10 : 0x20);
            b.ioWrite(0xFF00, 0x30);
        }
    b.ioWrite(0xFF00, 0x10);            // stop bit = 0
    b.ioWrite(0xFF00, 0x30);
}

int main() {
    Bus& b = g_bus;

    // --- Prepara datos de tiles en VRAM 0x8800 (tile 1 = color index 1) ------
    b.vram.fill(0);
    for (int row = 0; row < 8; ++row) {
        b.vram[0x800 + 32 + row * 2] = 0xFF;     // plano bits 0-1 -> bit0 = 1
        b.vram[0x800 + 32 + row * 2 + 1] = 0x00; // plano bits 2-3 -> bit1 = 0
    }
    uint8_t chr[16] = {0}; chr[0] = 0x99; chr[1] = 0x00;   // $13 CHR_TRN, mitad 0
    sgbSendPacket(b, chr);
    check(b.sgbHasTiles, "CHR_TRN carga los tiles");

    // --- Datos de tilemap + paletas, luego PCT_TRN ---------------------------
    b.vram.fill(0);
    // Entrada (0,0): tile 1, paleta 0  => e = 1 | (0<<10)
    uint16_t e = 1 | (0 << 10);
    b.vram[0x800] = e & 0xFF; b.vram[0x801] = e >> 8;
    // Paleta 0 (la primera del borde), color 1 = rojo puro (RGB555 0x001F)
    size_t palOff = 0x1000 + 1 * 2;
    b.vram[palOff] = 0x1F; b.vram[palOff + 1] = 0x00;
    uint8_t pct[16] = {0}; pct[0] = 0xA1;                  // $14 PCT_TRN
    sgbSendPacket(b, pct);

    check(b.sgbHasMap, "PCT_TRN carga el tilemap");
    check(b.sgbBorderFromPacket, "el borde se marca como recibido");
    check(b.sgbDecodedBorder[0] == 0xF80000u, "pixel (0,0) es rojo (paleta 0, color 1)");
    check(b.sgbDecodedBorder[8] == 0x000000u, "pixel (8,0) queda transparente");

    // --- Un paquete desconocido no debe romper nada --------------------------
    uint8_t unknown[16] = {0}; unknown[0] = 0x21;          // comando $04
    sgbSendPacket(b, unknown);
    check(b.sgbBorderFromPacket, "un comando desconocido no descarta el borde");

    // --- Deteccion SGB: C=0x14 y IDs de joypad via MLT_REQ -------------------
    g_model_pref = SYS_GB_SGB;
    b.reset();
    check(b.c == 0x14, "modo SGB expone la firma C=0x14");
    g_model_pref = SYS_AUTO;
    b.reset();
    check(b.c == 0x13, "modo Auto conserva la firma DMG C=0x13");

    // MLT_REQ (2 jugadores) => P1 devuelve IDs $xF, $xE al deseleccionar.
    b.ioWrite(0xFF00, 0x30); b.ioWrite(0xFF00, 0x00);
    uint8_t mlt[16] = {0}; mlt[0] = 0x89; mlt[1] = 0x01;   // $11 MLT_REQ, 2P
    for (int i = 0; i < 16; ++i)
        for (int bit = 0; bit < 8; ++bit) {
            b.ioWrite(0xFF00, ((mlt[i] >> bit) & 1) ? 0x10 : 0x20);
            b.ioWrite(0xFF00, 0x30);
        }
    b.ioWrite(0xFF00, 0x10); b.ioWrite(0xFF00, 0x30);
    b.ioWrite(0xFF00, 0x30);
    uint8_t id1 = b.readJoyp() & 0x0F;
    b.ioWrite(0xFF00, 0x10);   // P15 LOW
    b.ioWrite(0xFF00, 0x30);   // P15 LOW->HIGH: avanza de jugador
    uint8_t id2 = b.readJoyp() & 0x0F;
    check(id1 == 0x0F, "MLT_REQ: primer joypad devuelve ID 0x0F");
    check(id2 == 0x0E, "MLT_REQ: segundo joypad devuelve ID 0x0E");

    // --- reset() limpia el estado SGB ---------------------------------------
    b.reset();
    check(!b.sgbBorderFromPacket && !b.sgbHasTiles, "reset limpia el estado SGB");

    // --- Multiplicador de velocidad de audio (waveOutSetPlaybackRate) --------
    // 1.0 = 0x10000; 4x = 0x40000. No son Hz.
    check(MultiplicadorTasaAudio(44100, 44100) == 0x10000, "tasa de audio normal = 1.0 (0x10000)");
    check(MultiplicadorTasaAudio(44100 * 4, 44100) == 0x40000, "tasa de speedup 4x = 0x40000");
    check(MultiplicadorTasaAudio(22050, 44100) == 0x8000, "mitad de tasa = 0.5 (0x8000)");
    check(MultiplicadorTasaAudio(88200, 44100) == 0x20000, "doble de tasa = 2.0 (0x20000)");

    // --- Efectos de audio (submenu Audio) -----------------------------------
    check(VolumenIndice(0) == 0 && VolumenIndice(100) == 3 && VolumenIndice(200) == 5,
          "VolumenIndice mapea 0/100/200 a 0/3/5");

    {   // Volumen 200% duplica la senal.
        AudioFx fx; fx.mode = AUDIO_FX_NONE; fx.volume = 200;
        int16_t buf[2] = {1000, -1000};
        fx.process(buf, 2);
        check(buf[0] == 2000 && buf[1] == -2000, "volumen 200% duplica la senal");
    }
    {   // Volumen 0% silencia.
        AudioFx fx; fx.mode = AUDIO_FX_NONE; fx.volume = 0;
        int16_t buf[2] = {1000, -1000};
        fx.process(buf, 2);
        check(buf[0] == 0 && buf[1] == 0, "volumen 0% silencia");
    }
    {   // LPF1 atenua un escalon (primer valor < entrada) y converge.
        AudioFx fx; fx.mode = AUDIO_FX_LPF1; fx.volume = 100;
        int16_t buf[2] = {1000, 1000};
        fx.process(buf, 2);
        check(buf[0] < 1000 && buf[0] > 0, "LPF1 suaviza el primer valor");
        for (int i = 0; i < 200; ++i) { int16_t b[2] = {1000, 1000}; fx.process(b, 2); }
        int16_t b[2] = {1000, 1000}; fx.process(b, 2);
        check(b[0] >= 990, "LPF1 converge al valor de entrada");
    }
    {   // Reverb: un impulso deja cola en muestras posteriores.
        AudioFx fx; fx.mode = AUDIO_FX_REVERB; fx.volume = 100;
        int16_t imp[2] = {10000, 10000};
        fx.process(imp, 2);
        int tail = 0;
        for (int i = 0; i < 4096; ++i) { int16_t b[2] = {0, 0}; fx.process(b, 2); if (b[0] != 0) tail++; }
        check(tail > 0, "Reverb deja cola tras el impulso");
    }

    // --- Filtros de video (submenu Filter) ----------------------------------
    check(FILTER_COUNT == 12, "hay 12 filtros de video");
    check(kFilterMag[FILTER_NONE] == 1 && kFilterMag[FILTER_2XSAI] == 2 &&
          kFilterMag[FILTER_SIMPLE3X] == 3 && kFilterMag[FILTER_SIMPLE4X] == 4,
          "multiplicadores de filtro correctos");

    {   // Simple 2X: duplica cada pixel.
        static const int W = 3, H = 2;
        uint32_t src[W * H] = { 0xFF0000, 0x00FF00, 0x0000FF,
                                0xFFFFFF, 0x808080, 0x010203 };
        uint32_t dst[W * 2 * H * 2] = {0};
        g_screenBuffer[0] = src[0];
        filtSimpleNx(src, W, H, 2, dst);
        check(dst[0] == 0xFF0000 && dst[1] == 0xFF0000 && dst[W * 2] == 0xFF0000,
              "Simple 2X duplica el pixel 0");
        check(dst[2] == 0x00FF00 && dst[5] == 0x0000FF && dst[W * 2] == 0xFF0000,
              "Simple 2X conserva los colores");
    }
    {   // Simple 3X: magnitud 3.
        static const int W = 2, H = 1;
        uint32_t src[W] = { 0x112233, 0x445566 };
        uint32_t dst[W * 3 * 3] = {0};
        filtSimpleNx(src, W, H, 3, dst);
        check(dst[0] == 0x112233 && dst[1] == 0x112233 && dst[2] == 0x112233 &&
              dst[3] == 0x445566 && dst[4] == 0x445566,
              "Simple 3X duplica cada pixel 3 veces");
    }
    {   // filtAvg: promedio por canal.
        check(filtAvg(0xFF0000, 0x0000FF) == 0x7F007F, "filtAvg promedia por canal");
        check(filtAvg4(0xFF0000, 0xFF0000, 0x000000, 0x000000) == 0x7F0000,
              "filtAvg4 promedia cuatro pixeles");
    }
    {   // Bilinear: fila 0 es original + promedio horizontal, fila 1 interpola.
        static const int W = 2, H = 1;
        uint32_t src[W] = { 0xFF0000, 0x0000FF };
        uint32_t dst[W * 2 * 2] = {0};
        filtBilinear(src, W, H, dst);
        check(dst[0] == 0xFF0000, "Bilinear: pixel esquina = original");
        check(dst[1] == filtAvg(0xFF0000, 0x0000FF), "Bilinear: prom horizontal");
        // fila 1 columna 0 (indice = W*2 = 4): promedio vertical de a,c (borde)
        check(dst[W * 2] == filtAvg(0xFF0000, 0xFF0000), "Bilinear: prom vertical (borde)");
    }
    {   // Scanlines: pares duplicados, impares negros.
        static const int W = 2, H = 2;
        uint32_t src[W * H] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
        uint32_t dst[W * 2 * H * 2] = {1};
        filtScanlines(src, W, H, dst);
        check(dst[0] == 0xFF0000 && dst[1] == 0xFF0000, "Scanlines: fila par duplicada");
        check(dst[W * 2] == 0 && dst[W * 2 + 1] == 0, "Scanlines: fila impar negra");
    }
    {   // Simple 4X dimensiones y canal.
        static const int W = 1, H = 1;
        uint32_t src[1] = { 0xABCDEF };
        uint32_t dst[16] = {0};
        filtSimpleNx(src, W, H, 4, dst);
        bool all = true;
        for (int i = 0; i < 16; ++i) if (dst[i] != 0xABCDEF) { all = false; break; }
        check(all, "Simple 4X rellena 4x4");
    }
    {   // AplicarFiltroVideo consults g_screenBuffer when no SGB border.
        g_video_filter = FILTER_NONE;
        g_sgb_border_active = false;
        AplicarFiltroVideo();
        check(g_displayW == 0 && g_displayH == 0, "filtro None deja g_display en 0");
        g_video_filter = FILTER_SIMPLE2X;
        memset(g_screenBuffer, 0, sizeof(g_screenBuffer));
        g_screenBuffer[0] = 0x123456;
        AplicarFiltroVideo();
        check(g_displayW == GB_WIDTH * 2 && g_displayH == GB_HEIGHT * 2,
              "filtro 2X duplica las dimensiones de presentacion");
        check(g_displayBuffer[0] == 0x123456 && g_displayBuffer[1] == 0x123456,
              "AplicarFiltroVideo rellena el buffer");
        g_video_filter = FILTER_NONE;
    }

    // --- RTC real (MBC3) ----------------------------------------------------
    {   // rtcSetFromWallClock deja los registros dentro de rango y marca init.
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.ramBank = 0x08;
        r.rtcInitialized = false;
        r.rtcSetFromWallClock();
        check(r.rtcInitialized, "RTC se inicializa desde el reloj del sistema");
        check(r.rtcRegs[0] < 60 && r.rtcRegs[1] < 60 && r.rtcRegs[2] < 24,
              "RTC: segundos/minutos/horas dentro de rango");
        check(r.rtcLatched[0] == r.rtcRegs[0], "RTC: el latch arranca igual a los registros");
    }
    {   // rtcSync avanza por el tiempo transcurrido (base retrasada 1 hora).
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.ramBank = 0x08;
        r.rtcRegs[0] = 0; r.rtcRegs[1] = 0; r.rtcRegs[2] = 0; r.rtcRegs[3] = 0; r.rtcRegs[4] = 0;
        r.rtcInitialized = true;
        r.rtcBase = r.rtcNow() - 3661;   // 1 h, 1 min, 1 s
        r.rtcSync();
        check(r.rtcRegs[0] == 1 && r.rtcRegs[1] == 1 && r.rtcRegs[2] == 1,
              "RTC: rtcSync aplica el tiempo transcurrido (1h1m1s)");
    }
    {   // 24 horas => +1 dia, y el reloj vuelve a 0h.
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.ramBank = 0x08;
        r.rtcRegs[0] = 0; r.rtcRegs[1] = 0; r.rtcRegs[2] = 0; r.rtcRegs[3] = 0; r.rtcRegs[4] = 0;
        r.rtcInitialized = true;
        r.rtcBase = r.rtcNow() - 86400;
        r.rtcSync();
        check(r.rtcRegs[2] == 0 && (r.rtcRegs[3] | ((r.rtcRegs[4] & 1) << 8)) == 1,
              "RTC: 24h => +1 dia");
    }
    {   // La bandera halt congela el reloj.
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.ramBank = 0x08;
        r.rtcRegs[0] = 10; r.rtcRegs[1] = 0; r.rtcRegs[2] = 0; r.rtcRegs[3] = 0;
        r.rtcRegs[4] = 0x40;   // halt
        r.rtcInitialized = true;
        r.rtcBase = r.rtcNow() - 7200;
        r.rtcSync();
        check(r.rtcRegs[0] == 10 && r.rtcRegs[2] == 0, "RTC: halt congela el reloj");
    }
    {   // Lectura/escritura por los registros del cartucho (ramBank 8..0xC).
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.rtcInitialized = true;
        r.ramBank = 0x08;
        r.cartRamWrite(0xA000, 30);       // segundos
        check(r.rtcRegs[0] == 30, "RTC: escritura del juego a los segundos");
        r.rtcBase = r.rtcNow();
        r.ramBank = 0x0A;                 // horas
        r.cartRamWrite(0xA000, 7);
        r.ramBank = 0x08;
        r.rtcLatch();
        check((r.cartRamRead(0xA000) & 0x3F) == 30, "RTC: lectura del latch (segundos)");
        r.ramBank = 0x0A;
        check((r.cartRamRead(0xA000) & 0x1F) == 7, "RTC: lectura del latch (horas)");
    }
    {   // El latch solo se refresca con el comando 0x00->0x01.
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.rtcInitialized = true;
        r.rtcRegs[0] = 5; r.rtcLatched[0] = 5; r.ramBank = 0x08;
        r.rtcBase = r.rtcNow();
        r.rtcRegs[0] = 9;                 // el reloj avanza por detras
        check((r.cartRamRead(0xA000) & 0x3F) == 5, "RTC: sin latch, la lectura no cambia");
        r.writeByte(0x6000, 0x00);        // latch: 0x00
        r.writeByte(0x6000, 0x01);        // -> 0x01 dispara el latch
        check((r.cartRamRead(0xA000) & 0x3F) == 9, "RTC: el comando 0x00->0x01 refresca el latch");
    }
    {   // g_rtc_enabled=false congela el reloj (modo clasico).
        Bus r;
        r.mbcType = 3; r.hasRtc = true; r.ramEnabled = true; r.ramBank = 0x08;
        r.rtcRegs[0] = 0; r.rtcRegs[1] = 0; r.rtcRegs[2] = 0; r.rtcRegs[3] = 0; r.rtcRegs[4] = 0;
        r.rtcInitialized = true;
        r.rtcBase = r.rtcNow() - 5000;
        g_rtc_enabled = false;
        r.rtcSync();
        check(r.rtcRegs[0] == 0 && r.rtcRegs[1] == 0 && r.rtcRegs[2] == 0,
              "RTC: desactivado no avanza el reloj");
        g_rtc_enabled = true;
    }

    std::printf(g_fail ? "\nRESULTADO: %d FALLOS\n" : "\nRESULTADO: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
