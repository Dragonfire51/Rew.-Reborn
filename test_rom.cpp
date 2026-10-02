// Prueba con ROMs reales: carga un cartucho en un modo de sistema y comprueba
// el arranque (firma SGB / modo CGB), la deteccion del juego y el borde SGB.
//
// Uso: test_rom <rom.gb> [modo] [frames]
//   modo: auto | dmg | sgb | gb_sgb | gbc_sgb | gbc_init_sgb
#define REW_SGB_TEST_HOOKS
#include "Rew. Reborn.cpp"
#include <unistd.h>
#include <cstdio>

// ---- enganches de diagnostico (definidos aqui, referenciados por el core) ---
uint8_t g_sgb_pkts[64][16] = {{0}};
int g_sgb_cmd_n = 0;
static uint8_t g_raw[600000];
static int g_raw_n = 0;
static bool g_raw_on = false;
void sgbRawLog(uint8_t v) {
    if (!g_raw_on) return;
    if (g_raw_n < 600000) g_raw[g_raw_n++] = v & 0x30;
}

static int ModeFromName(const char* s) {
    if (!std::strcmp(s, "dmg")) return SYS_DMG;
    if (!std::strcmp(s, "sgb")) return SYS_SGB;
    if (!std::strcmp(s, "gb_sgb")) return SYS_GB_SGB;
    if (!std::strcmp(s, "gbc_sgb")) return SYS_GBC_SGB;
    if (!std::strcmp(s, "gbc_init_sgb")) return SYS_GBC_INIT_SGB;
    return SYS_AUTO;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("uso: test_rom <rom> [modo] [frames]\n"); return 2; }
    int mode = argc > 2 ? ModeFromName(argv[2]) : SYS_GB_SGB;
    int maxFrames = argc > 3 ? atoi(argv[3]) : 3000;

    Bus& b = g_bus;
    g_model_pref = mode;
    if (!b.loadRom(argv[1])) { std::printf("No se pudo cargar la ROM\n"); return 2; }

    std::printf("ROM=%s modo=%s MBC=%d CGB=%d ram=0x%X\n",
                argv[1], g_system_mode_names[mode], b.mbcType, b.isCgbMode, b.ramSize);
    std::printf("arranque: C=%02X  (SGB=%s, CGB=%s)\n", b.c,
                b.c == 0x14 ? "si" : "no", b.isCgbMode ? "si" : "no");

    g_raw_on = true;
    int frames = 0;
    for (; frames < maxFrames; ++frames) b.runFrame();
    g_raw_on = false;

    int opaque = 0;
    for (int i = 0; i < SGB_W * SGB_H; ++i) if (b.sgbDecodedBorder[i]) opaque++;
    int nonblank = 0;
    for (int i = 0; i < GB_WIDTH * GB_HEIGHT; ++i)
        if (b.frameBuf[i] != b.frameBuf[0]) nonblank++;

    std::printf("frames=%d MLT_REQ=%d hasTiles=%d hasMap=%d fromPacket=%d\n",
                frames, b.sgbMultiPlayer, b.sgbHasTiles, b.sgbHasMap, b.sgbBorderFromPacket);
    std::printf("borde opaco=%d/%d  video no-blanco=%d  pkts=%d\n",
                opaque, SGB_W * SGB_H, nonblank, g_sgb_cmd_n);

    std::printf("paquetes:");
    for (int i = 0; i < g_sgb_cmd_n && i < 32; ++i)
        std::printf(" $%X", g_sgb_pkts[i][0] >> 3);
    std::printf("\n");

    if (b.sgbBorderFromPacket) {
        char path[256];
        std::snprintf(path, sizeof path, "/tmp/border_%d.bin", (int)getpid());
        FILE* f = fopen(path, "wb");
        fwrite(b.sgbDecodedBorder, 4, SGB_W * SGB_H, f);
        fclose(f);
        std::printf("borde guardado en %s\n", path);
    }
    { FILE* f = fopen("/tmp/raw_last.bin", "wb"); fwrite(g_raw, 1, g_raw_n, f); fclose(f); }

    return 0;
}
