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

    std::printf(g_fail ? "\nRESULTADO: %d FALLOS\n" : "\nRESULTADO: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
