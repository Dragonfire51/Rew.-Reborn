// ============================================================================
// Rew. Reborn - Game Boy / Game Boy Color emulator (Win32)
// RAMA windows-9x-32bit: port para Windows 95/98/98SE/ME y Windows de 32 bits
//   (binario i686). Compila con -DREW_WIN9X (ver Makefile.win9x). En 9x no
//   existen SetWindowLongPtrA/GetWindowLongPtrA (solo NT), asi que se mapean a
//   SetWindowLongA/GetWindowLongA; el nivel de API baja a _WIN32_WINNT 0x0400.
//   Requiere 32 bits: un PE32+ (64-bit) no arranca en Windows 9x.
// v0.8.3-alpha: corrige el tono del audio (la musica sonaba mas grave).
//   - waveOutSetPlaybackRate espera un multiplicador de punto fijo (1.0 =
//     0x10000), no una frecuencia en Hz; se pasaban 44100/176400 y el tono
//     bajaba tanto a velocidad normal como en speedup. Ahora se usa
//     MultiplicadorTasaAudio (44100 -> 0x10000, 176400 -> 0x40000).
// v0.8.2-alpha: bordes reales en los modos CGB+SGB, nombre de ROM en el
//   titulo y Speedup sincronizado con el audio.
//   - Los modos "Game Boy Color + SGB Border" y "... + Initial SGB Border"
//     muestran el borde real del juego (Pokemon Oro/Plata/Cristal, Telefang,
//     bootlegs, etc.) conservando el color CGB.
//   - El titulo de la ventana muestra el nombre del cartucho cargado.
//   - El Speedup mantiene el ritmo y acelera la reproduccion de audio a la
//     misma tasa (o lo silencia si el dispositivo no lo admite).
// v0.8.1-alpha: modos Super Game Boy con borde y Speedup Toggle.
//   - System: Auto, Game Boy (DMG), Super Game Boy, Game Boy + SGB Border,
//     Game Boy Color + SGB Border y Game Boy Color + Initial SGB Border.
//   - El borde SGB (256x224) se dibuja alrededor del area de juego 160x144.
//   - Speedup Toggle: tecla mapeable que acelera la emulacion mientras se
//     mantiene pulsada (disponible en los 4 perfiles de teclas).
// v0.7.0-alpha: core reescrito con temporización por M-ciclo.
//   - CPU SM83 con accesos a memoria ciclo a ciclo (timer/PPU/APU/DMA avanzan
//     en cada acceso), temporización de interrupciones, HALT y halt bug.
//   - Timer con detección de flanco y recarga retardada de TIMA.
//   - PPU por puntos (dots), interrupciones STAT por flanco, LY=153 quirk,
//     OAM DMA real, bug de corrupción de OAM (DMG), HDMA/GDMA.
//   - CGB: bancos de WRAM (SVBK), VRAM (VBK), paletas, doble velocidad.
//   - MBC1/MBC2/MBC3(+RTC)/MBC5 con máscaras de banco correctas.
//   - APU con secuenciador de frames ligado a DIV y comportamiento de hardware.
// Compilar con -DREW_HEADLESS para usar sólo el núcleo (tests).
// ============================================================================
#ifndef REW_HEADLESS
// Objetivo Windows 9x/ME y Windows de 32 bits: fija el nivel de API a Win95/NT4
// y mapea las funciones "pointer-width" de ventana a sus equivalentes de 32
// bits. En 9x USER32 solo expone SetWindowLongA/GetWindowLongA (las variantes
// *Ptr son exclusivas de NT), y en 32 bits LONG_PTR == LONG, así que el cast es
// seguro. En 64 bits se mantienen las funciones reales de USER32.
#ifdef REW_WIN9X
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include "resource.h"
#ifdef REW_WIN9X
#ifndef _WIN64
#define SetWindowLongPtrA SetWindowLongA
#define SetWindowLongPtrW SetWindowLongW
#define GetWindowLongPtrA GetWindowLongA
#define GetWindowLongPtrW GetWindowLongW
#endif
#endif
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <array>
#include <string>
#include <cstring>
#include <ctime>
#include <algorithm>

// Operadores new/delete propios. libstdc++'s eh_alloc.o (allocador de
// excepciones) arrastra libgcc gthr-win32.o, que importa GetThreadId (Vista+)
// y TryEnterCriticalSection (NT4+), inexistentes en Windows 9x/ME. Al aportar
// estos operadores no se enlazan eh_alloc.o/new_op.o/del_ops.o y el binario
// deja de importar esas funciones.
#ifdef REW_WIN9X
void* operator new(std::size_t n) { void* p = std::malloc(n ? n : 1); return p; }
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// Sustituye libgcc's gthr-win32.o, que importa GetThreadId (Vista+) y
// TryEnterCriticalSection (NT4+), inexistentes en Windows 9x/ME. En este
// enlace su unico consumidor es eh_alloc.o (el mutex del allocador de
// excepciones); aportando estas cuatro funciones no se enlaza gthr-win32.o.
// Los tipos reales son CRITICAL_SECTION*; se usa void* para evitar depender de
// la cabecera interna gthr-win32.h. InitializeCriticalSection/Enter/Leave/
// Delete existen desde Windows 95.
extern "C" void __gthr_win32_mutex_init_function(void* m) { InitializeCriticalSection((CRITICAL_SECTION*)m); }
extern "C" void __gthr_win32_mutex_lock(void* m) { EnterCriticalSection((CRITICAL_SECTION*)m); }
extern "C" void __gthr_win32_mutex_unlock(void* m) { LeaveCriticalSection((CRITICAL_SECTION*)m); }
extern "C" int  __gthr_win32_mutex_destroy(void* m) { DeleteCriticalSection((CRITICAL_SECTION*)m); return 0; }
#endif

// E/S de ficheros con FILE* en lugar de <fstream>. En este port (Windows
// 9x/ME) se evita <fstream>/<iostream> a proposito: arrastran las guardas de
// inicializacion de std::locale (guard.o -> __gthr_win32_cond_init_function),
// que importan InitializeConditionVariable/SleepConditionVariableCS/GetThreadId
// (solo Vista+). Con FILE* el binario no importa ninguna API posterior a 9x.
struct FOut {
    FILE* f = nullptr;
    bool fail = false;
    explicit FOut(const char* path) { f = fopen(path, "wb"); }
    ~FOut() { if (f) fclose(f); }
    bool ok() const { return f != nullptr; }
    bool good() const { return f != nullptr && !fail && !ferror(f); }
    void write(const void* p, size_t n) { if (f && fwrite(p, 1, n, f) != n) fail = true; }
};
struct FIn {
    FILE* f = nullptr;
    bool fail = false;
    explicit FIn(const char* path) { f = fopen(path, "rb"); }
    ~FIn() { if (f) fclose(f); }
    bool ok() const { return f != nullptr; }
    bool good() const { return f != nullptr && !fail && !ferror(f); }
    void read(void* p, size_t n) { if (f && !fail && fread(p, 1, n, f) != n) fail = true; }
};
// Lee un fichero entero a un vector (0 bytes si no existe).
inline std::vector<uint8_t> LeerFichero(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
    fclose(f);
    return v;
}

#ifndef REW_HEADLESS
// MAX_PATH lo aporta windows.h
#else
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#endif

const int GB_WIDTH = 160;
const int GB_HEIGHT = 144;
uint32_t g_screenBuffer[GB_WIDTH * GB_HEIGHT] = {0};
bool g_rom_loaded = false;

// 0 = automático (GBC si el cartucho lo soporta), 1 = forzar DMG
int g_model_pref = 0;

// ============================================================================
// [v0.8] Super Game Boy: borde 256x224 alrededor del area de juego 160x144.
// El borde se genera por procedimiento (sin tablas gigantes) a partir de un
// indice de estilo y una paleta.
// ============================================================================
#define SGB_W 256
#define SGB_H 224
#define SGB_VX 48
#define SGB_VY 40
#define SGB_VW 160
#define SGB_VH 144
#define SGB_BORDER_COUNT 12
#define SGB_SPEEDUP_MULT 4

uint32_t g_sgbBuffer[SGB_W * SGB_H] = {0};
bool g_sgb_border_active = false;
int g_sgb_border_index = 0;
bool g_speedup_held = false;
bool g_force_sgb_border = false;   // muestra borde tambien si el juego no trae uno
int  g_sgb_theme_pref = -1;        // -1 = automatico segun el modo; >=0 = estilo fijo
bool g_sgb_border_checked = false; // ya se aplico el borde real de la ROM actual
// Captura del borde real para los modos CGB+SGB: se ejecuta el juego en un
// modo DMG+SGB desechable hasta que envia su borde, y luego se restaura la
// partida CGB con color mostrando ese borde. `g_sgb_capture_pending` queda a
// true mientras la partida en curso aun no tiene su borde real.
bool g_sgb_capture_pending = false;
uint32_t g_sgb_captured_border[SGB_W * SGB_H] = {0};
int  g_sgb_captured_count = 0;
char g_rom_display_name[MAX_PATH] = {0};   // nombre del cartucho para el titulo
char g_window_base_title[MAX_PATH] = "Rew. Reborn v0.8.3-alpha";
// Speedup sincronizado con el audio: waveOut reproduce M veces mas rapido, de
// modo que el sonido conserva su tono mientras la emulacion acelera. Si el
// dispositivo no admite el cambio de frecuencia, se silencia mientras dura el
// speedup en lugar de perder el sincronismo.
bool  g_speedup_rate_active = false;    // waveOut ya esta a la frecuencia M
bool  g_speedup_audio_muted = false;    // el dispositivo no acelera: se silencia
bool  g_speedup_rate_supported = true;
bool  g_speedup_prev = false;           // estado de speedup en el fotograma anterior

// Modos de sistema (System menu). 0 y 1 ya existian.
enum { SYS_AUTO = 0, SYS_DMG = 1, SYS_SGB = 2, SYS_GB_SGB = 3, SYS_GBC_SGB = 4, SYS_GBC_INIT_SGB = 5 };
const int SYSTEM_MODE_COUNT = 6;

const char* g_system_mode_names[SYSTEM_MODE_COUNT] = {
    "Auto (GBC si el cartucho lo soporta)",
    "Game Boy (DMG)",
    "Super Game Boy",
    "Game Boy + SGB Border",
    "Game Boy Color + SGB Border",
    "Game Boy Color + Initial SGB Border"
};

// ---------------------------------------------------------------------------
// Bordes tematicos de reserva (fallback). El borde autentico lo envia la ROM
// por CHR_TRN/PCT_TRN; si el juego no incluye uno, se usa el estilo del modo.
// Los temas conmutables reproducen los bordes vistos en la galeria de
// vgmuseum (Pokemon Yellow, Gold/Silver, etc.).
// ---------------------------------------------------------------------------
#define SGB_THEME_COUNT 8

struct SgbTheme { const char* name; uint32_t base, accent, extra; };
static const SgbTheme kSgbThemes[SGB_THEME_COUNT] = {
    { "Por defecto",   0x505860, 0xC8D0D8, 0x303840 },
    { "Game Boy",      0x186028, 0x88C860, 0x0C3818 },
    { "Pokemon Yellow",0xE8C020, 0x804000, 0xF8F0A0 },
    { "Pokemon Gold",  0xD8A020, 0x704810, 0xF8E8B0 },
    { "Pokemon Silver",0xB8C0C8, 0x505870, 0xE8F0F8 },
    { "Pokemon Red",   0xC02820, 0x601010, 0xF0C0B0 },
    { "Pokemon Blue",  0x2848A0, 0x101860, 0xB0C8F0 },
    { "Zelda",         0x287038, 0xE8D060, 0x104020 }
};

// Paletas (base, acento) por modo. En los modos SGB se usan 8 estilos.
static const uint32_t kSgbBase[SYSTEM_MODE_COUNT] = {
    0x000000, 0x000000, 0x505860, 0x186028, 0x303088, 0x707018
};
static const uint32_t kSgbAccent[SYSTEM_MODE_COUNT] = {
    0x000000, 0x000000, 0xC8D0D8, 0x88C860, 0xFFD040, 0xF0E0A0
};

bool ModeUsesBorder(int mode) { return mode >= SYS_SGB; }
bool ModeIsSgb(int mode) { return mode == SYS_SGB || mode == SYS_GB_SGB; }
// El modo CGB se decide por la ROM; Initial SGB Border fuerza GBC.
bool ModeForcesCgb(int mode) { return mode == SYS_GBC_SGB || mode == SYS_GBC_INIT_SGB; }

void GenerarBordeSgb(int index, uint32_t base, uint32_t accent, uint32_t extra = 0);

// Estilo de reserva asociado al modo de sistema.
int SgbThemeForMode(int mode) {
    switch (mode) {
        case SYS_GB_SGB:       return 1;   // Game Boy
        case SYS_GBC_SGB:      return 2;   // Pokemon Yellow
        case SYS_GBC_INIT_SGB: return 3;   // Pokemon Gold
        default:               return 0;
    }
}
void GenerarBordeSgb(int index, uint32_t base, uint32_t accent, uint32_t extra) {
    auto ch = [](uint32_t c, int sh) { return (uint8_t)((c >> sh) & 0xFF); };
    int br = ch(base, 16), bg = ch(base, 8), bb = ch(base, 0);
    int ar = ch(accent, 16), ag = ch(accent, 8), ab = ch(accent, 0);
    int xr = extra ? ch(extra, 16) : br, xg = extra ? ch(extra, 8) : bg, xb = extra ? ch(extra, 0) : bb;
    auto mix = [](int a, int b, int t) { return (a * (255 - t) + b * t) / 255; };
    auto dark = [&](int v) { return v * 55 / 100; };
    int dbr = dark(br), dbg = dark(bg), dbb = dark(bb);

    for (int y = 0; y < SGB_H; ++y) {
        for (int x = 0; x < SGB_W; ++x) {
            // Fondo con damero desplazado segun el estilo.
            int cell = ((x + index * 4) >> 3) + ((y + index * 2) >> 3);
            int r = (cell & 1) ? mix(br, ar, 90) : br;
            int g = (cell & 1) ? mix(bg, ag, 90) : bg;
            int b = (cell & 1) ? mix(bb, ab, 90) : bb;
            // Rejilla cada 16 px.
            if ((x % 16) == 0 || (x % 16) == 15 || (y % 16) == 0 || (y % 16) == 15) {
                r = dbr; g = dbg; b = dbb;
            }
            // Motivo diagonal suave (da caracter al estilo).
            if (((x + y) % 24) == 0) { r = mix(r, xr, 70); g = mix(g, xg, 70); b = mix(b, xb, 70); }
            // Marco exterior de 2 px.
            if (x < 2 || y < 2 || x >= SGB_W - 2 || y >= SGB_H - 2) { r = ar; g = ag; b = ab; }
            g_sgbBuffer[y * SGB_W + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    // Marco interior alrededor del area de juego (6 px, oscuro + acento).
    for (int y = SGB_VY - 6; y < SGB_VY + SGB_VH + 6; ++y) {
        for (int x = SGB_VX - 6; x < SGB_VX + SGB_VW + 6; ++x) {
            if (x < 0 || y < 0 || x >= SGB_W || y >= SGB_H) continue;
            bool inside = x >= SGB_VX && x < SGB_VX + SGB_VW && y >= SGB_VY && y < SGB_VY + SGB_VH;
            if (inside) continue;
            bool inner = x >= SGB_VX - 2 && x < SGB_VX + SGB_VW + 2 &&
                         y >= SGB_VY - 2 && y < SGB_VY + SGB_VH + 2;
            uint32_t c = inner ? accent : ((uint32_t)dbr << 16) | ((uint32_t)dbg << 8) | dbb;
            g_sgbBuffer[y * SGB_W + x] = c;
        }
    }

    // Adornos en las cuatro esquinas del area de juego.
    const int cx[4] = {SGB_VX - 14, SGB_VX + SGB_VW + 4, SGB_VX - 14, SGB_VX + SGB_VW + 4};
    const int cy[4] = {SGB_VY - 14, SGB_VY - 14, SGB_VY + SGB_VH + 4, SGB_VY + SGB_VH + 4};
    for (int k = 0; k < 4; ++k)
        for (int dy = 0; dy < 10; ++dy)
            for (int dx = 0; dx < 10; ++dx) {
                int x = cx[k] + dx, y = cy[k] + dy;
                if (x < 0 || y < 0 || x >= SGB_W || y >= SGB_H) continue;
                g_sgbBuffer[y * SGB_W + x] = ((dx + dy) % 4 < 2) ? accent
                    : ((uint32_t)dbr << 16) | ((uint32_t)dbg << 8) | dbb;
            }
}

// Copia el frame 160x144 al centro del borde para presentarlo.
// (Definido despues de la instancia global `g_bus`.)

// ============================================================================
// APU
// Se avanza en pasos de 2 T-ciclos (2 MHz) sincronizado con la CPU.
// El secuenciador de frames (512 Hz) lo dispara el flanco de bajada del bit
// 12 de DIV (bit 13 en doble velocidad), igual que en hardware.
// ============================================================================
class APU {
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int BUF_SAMPLES = 4096;          // pares L/R
    int16_t sampleBuf[BUF_SAMPLES * 2] = {0};
    int sampleCount = 0;                               // int16 escritos (L+R)

    bool cgb = false;
    bool power = true;
    uint8_t reg[0x17] = {0};                           // FF10..FF26 (valores escritos)
    uint8_t waveRam[16] = {0};

    struct Pulse {
        bool on, dac, lenEn, envUp, envDone;
        int len, freq, timer, pos, duty, vol, envTimer, envPeriod;
    };
    struct Wave {
        bool on, dac, lenEn, justRead;
        int len, freq, timer, pos, volCode;
        uint8_t buf;
    };
    struct Noise {
        bool on, dac, lenEn, envUp, envDone;
        int len, timer, vol, envTimer, envPeriod;
        uint16_t lfsr;
    };
    Pulse sq[2] = {};
    Wave wv = {};
    Noise nz = {};
    bool swEn = false, swNegUsed = false;
    int swTimer = 0, swShadow = 0;
    int fsStep = 0;                                    // próximo paso del secuenciador

    // mezcla / remuestreo
    int64_t accL = 0, accR = 0;
    int accN = 0;
    int64_t samplePhase = 0;
    double capL = 0.0, capR = 0.0;

    static constexpr uint8_t kReadMask[0x17] = {
        0x80,0x3F,0x00,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF,
        0x00,0x00,0x70
    };
    static constexpr uint8_t kDuty[4][8] = {
        {0,0,0,0,0,0,0,1},{1,0,0,0,0,0,0,1},{1,0,0,0,0,1,1,1},{0,1,1,1,1,1,1,0}
    };

    void reset(bool cgbMode) {
        cgb = cgbMode;
        power = true;
        memset(reg, 0, sizeof(reg));
        sq[0] = Pulse{}; sq[1] = Pulse{}; wv = Wave{}; nz = Noise{};
        nz.lfsr = 0x7FFF;
        swEn = swNegUsed = false; swTimer = swShadow = 0;
        fsStep = 0;
        accL = accR = 0; accN = 0; samplePhase = 0; capL = capR = 0.0;
        sampleCount = 0;
        static const uint8_t wavInit[16] = {0x84,0x40,0x43,0xAA,0x2D,0x78,0x92,0x3C,
                                            0x60,0x59,0x59,0xB0,0x34,0xB8,0x2E,0xDA};
        if (cgb) { for (int i = 0; i < 16; ++i) waveRam[i] = (i & 1) ? 0xFF : 0x00; }
        else memcpy(waveRam, wavInit, 16);
        // estado tras el boot ROM
        reg[0x00]=0x80; reg[0x01]=0xBF; reg[0x02]=0xF3; reg[0x03]=0xFF; reg[0x04]=0xBF;
        reg[0x06]=0x3F; reg[0x07]=0x00; reg[0x08]=0xFF; reg[0x09]=0xBF;
        reg[0x0A]=0x7F; reg[0x0B]=0xFF; reg[0x0C]=0x9F; reg[0x0D]=0xFF; reg[0x0E]=0xBF;
        reg[0x10]=0xFF; reg[0x11]=0x00; reg[0x12]=0x00; reg[0x13]=0xBF;
        reg[0x14]=0x77; reg[0x15]=0xF3;
        sq[0].duty = 2; sq[0].dac = true; sq[0].on = true; sq[0].vol = 0; sq[0].envDone = true;
        sq[0].freq = 0x7FF; sq[0].timer = 4;
        sq[1].freq = 0x7FF; sq[1].timer = 4;
        wv.freq = 0x7FF; wv.timer = 2;
        nz.timer = 8;
    }

    // ----------------------------------------------------------------- helpers
    int noisePeriod() const {
        static const int div[8] = {8,16,32,48,64,80,96,112};
        uint8_t n43 = reg[0x12];
        return div[n43 & 7] << (n43 >> 4);
    }
    int sweepCalc() {
        uint8_t n10 = reg[0x00];
        int d = swShadow >> (n10 & 7);
        int nf;
        if (n10 & 0x08) { nf = swShadow - d; swNegUsed = true; }
        else nf = swShadow + d;
        if (nf > 2047) sq[0].on = false;
        return nf;
    }
    template <class Ch> static void clockLen(Ch& ch) {
        if (ch.lenEn && ch.len > 0) { if (--ch.len == 0) ch.on = false; }
    }
    template <class Ch> static void clockEnv(Ch& ch) {
        if (--ch.envTimer <= 0) {
            ch.envTimer = ch.envPeriod ? ch.envPeriod : 8;
            if (ch.envPeriod && !ch.envDone) {
                int nv = ch.vol + (ch.envUp ? 1 : -1);
                if (nv >= 0 && nv <= 15) ch.vol = nv; else ch.envDone = true;
            }
        }
    }
    void frameSeqClock() {
        if (!power) return;
        int step = fsStep;
        fsStep = (fsStep + 1) & 7;
        if ((step & 1) == 0) { clockLen(sq[0]); clockLen(sq[1]); clockLen(wv); clockLen(nz); }
        if (step == 2 || step == 6) {
            if (--swTimer <= 0) {
                uint8_t n10 = reg[0x00];
                int period = (n10 >> 4) & 7;
                swTimer = period ? period : 8;
                if (swEn && period) {
                    int nf = sweepCalc();
                    if (nf <= 2047 && (n10 & 7)) {
                        swShadow = nf;
                        sq[0].freq = nf;
                        reg[0x03] = nf & 0xFF;
                        reg[0x04] = (reg[0x04] & 0xF8) | ((nf >> 8) & 7);
                        sweepCalc();
                    }
                }
            }
        }
        if (step == 7) { clockEnv(sq[0]); clockEnv(sq[1]); clockEnv(nz); }
    }

    // Longitud + trigger comunes a NRx4. Devuelve true si hay trigger.
    template <class Ch> bool lenTrig(Ch& ch, int maxLen, uint8_t v) {
        bool was = ch.lenEn;
        ch.lenEn = (v & 0x40) != 0;
        bool nextNoLen = (fsStep & 1) != 0;
        if (nextNoLen && !was && ch.lenEn && ch.len > 0) {
            if (--ch.len == 0 && !(v & 0x80)) ch.on = false;
        }
        if (v & 0x80) {
            if (ch.len == 0) { ch.len = maxLen; if (ch.lenEn && nextNoLen) ch.len--; }
            return true;
        }
        return false;
    }
    template <class Ch> void envTrigger(Ch& ch, uint8_t nrx2) {
        ch.vol = nrx2 >> 4;
        ch.envUp = (nrx2 & 0x08) != 0;
        ch.envPeriod = nrx2 & 7;
        ch.envTimer = ch.envPeriod ? ch.envPeriod : 8;
        if (fsStep == 7) ch.envTimer++;
        ch.envDone = false;
    }

    void powerOff() {
        int l0 = sq[0].len, l1 = sq[1].len, l2 = wv.len, l3 = nz.len;
        for (int i = 0; i < 0x16; ++i) reg[i] = 0;
        sq[0] = Pulse{}; sq[1] = Pulse{}; wv = Wave{}; nz = Noise{};
        nz.lfsr = 0x7FFF;
        swEn = swNegUsed = false; swTimer = swShadow = 0;
        if (!cgb) { sq[0].len = l0; sq[1].len = l1; wv.len = l2; nz.len = l3; }
        sq[0].timer = sq[1].timer = 8192; wv.timer = 4096; nz.timer = 8;
        power = false;
    }
    void powerOn() {
        power = true;
        fsStep = 0;
        sq[0].pos = sq[1].pos = 0;
        wv.buf = 0;
    }

    // ----------------------------------------------------------------- bus
    uint8_t read(uint16_t addr) const {
        if (addr >= 0xFF30) {
            int i = addr - 0xFF30;
            if (wv.on) {
                if (!cgb && !wv.justRead) return 0xFF;
                return waveRam[wv.pos >> 1];
            }
            return waveRam[i];
        }
        if (addr == 0xFF26) {
            return (uint8_t)((power ? 0x80 : 0) | 0x70 | (sq[0].on ? 1 : 0) | (sq[1].on ? 2 : 0) |
                             (wv.on ? 4 : 0) | (nz.on ? 8 : 0));
        }
        int i = addr - 0xFF10;
        if (i < 0 || i >= 0x17) return 0xFF;
        return reg[i] | kReadMask[i];
    }

    void write(uint16_t addr, uint8_t v) {
        if (addr >= 0xFF30) {
            int i = addr - 0xFF30;
            if (wv.on) {
                if (!cgb && !wv.justRead) return;
                waveRam[wv.pos >> 1] = v;
                return;
            }
            waveRam[i] = v;
            return;
        }
        if (addr == 0xFF26) {
            bool np = (v & 0x80) != 0;
            if (!np && power) powerOff();
            else if (np && !power) powerOn();
            return;
        }
        int i = addr - 0xFF10;
        if (i < 0 || i >= 0x16) return;
        if (!power) {
            if (!cgb) {
                if (addr == 0xFF11) sq[0].len = 64 - (v & 63);
                else if (addr == 0xFF16) sq[1].len = 64 - (v & 63);
                else if (addr == 0xFF1B) wv.len = 256 - v;
                else if (addr == 0xFF20) nz.len = 64 - (v & 63);
            }
            return;
        }
        uint8_t old = reg[i];
        reg[i] = v;
        switch (addr) {
            case 0xFF10:
                if (swNegUsed && (old & 0x08) && !(v & 0x08)) sq[0].on = false;
                break;
            case 0xFF11: case 0xFF16: {
                Pulse& p = sq[addr == 0xFF11 ? 0 : 1];
                p.duty = v >> 6; p.len = 64 - (v & 63);
                break;
            }
            case 0xFF12: case 0xFF17: {
                Pulse& p = sq[addr == 0xFF12 ? 0 : 1];
                p.dac = (v & 0xF8) != 0;
                if (!p.dac) p.on = false;
                break;
            }
            case 0xFF13: case 0xFF18: {
                Pulse& p = sq[addr == 0xFF13 ? 0 : 1];
                p.freq = (p.freq & 0x700) | v;
                break;
            }
            case 0xFF14: case 0xFF19: {
                int k = addr == 0xFF14 ? 0 : 1;
                Pulse& p = sq[k];
                p.freq = (p.freq & 0xFF) | ((v & 7) << 8);
                if (lenTrig(p, 64, v)) {
                    p.on = true;
                    p.timer = (2048 - p.freq) * 4 + 8;
                    envTrigger(p, reg[k == 0 ? 0x02 : 0x07]);
                    if (k == 0) {
                        uint8_t n10 = reg[0x00];
                        int period = (n10 >> 4) & 7, shift = n10 & 7;
                        swShadow = p.freq;
                        swTimer = period ? period : 8;
                        swEn = period || shift;
                        swNegUsed = false;
                        if (shift) sweepCalc();
                    }
                    if (!p.dac) p.on = false;
                }
                break;
            }
            case 0xFF1A:
                wv.dac = (v & 0x80) != 0;
                if (!wv.dac) wv.on = false;
                break;
            case 0xFF1B: wv.len = 256 - v; break;
            case 0xFF1C: wv.volCode = (v >> 5) & 3; break;
            case 0xFF1D: wv.freq = (wv.freq & 0x700) | v; break;
            case 0xFF1E: {
                wv.freq = (wv.freq & 0xFF) | ((v & 7) << 8);
                if (lenTrig(wv, 256, v)) {
                    // DMG: re-disparar justo cuando el canal lee RAM de onda la corrompe
                    if (!cgb && wv.on && wv.timer <= 2) {
                        int off = ((wv.pos + 1) >> 1) & 0xF;
                        if (off < 4) waveRam[0] = waveRam[off];
                        else memcpy(waveRam, waveRam + (off & ~3), 4);
                    }
                    wv.on = true;
                    wv.pos = 0;
                    wv.timer = (2048 - wv.freq) * 2 + 6;
                    if (!wv.dac) wv.on = false;
                }
                break;
            }
            case 0xFF20: nz.len = 64 - (v & 63); break;
            case 0xFF21:
                nz.dac = (v & 0xF8) != 0;
                if (!nz.dac) nz.on = false;
                break;
            case 0xFF23:
                if (lenTrig(nz, 64, v)) {
                    nz.on = true;
                    nz.lfsr = 0x7FFF;
                    nz.timer = noisePeriod() + 8;
                    envTrigger(nz, reg[0x11]);
                    if (!nz.dac) nz.on = false;
                }
                break;
            default: break;
        }
    }

    // ----------------------------------------------------------------- output
    int pulseOut(int k) const {
        const Pulse& p = sq[k];
        return (p.on && kDuty[p.duty][p.pos]) ? p.vol : 0;
    }
    int waveOut() const {
        if (!wv.on || wv.volCode == 0) return 0;
        uint8_t s = (wv.pos & 1) ? (wv.buf & 0x0F) : (wv.buf >> 4);
        return s >> (wv.volCode - 1);
    }
    int noiseOut() const { return (nz.on && !(nz.lfsr & 1)) ? nz.vol : 0; }
    uint8_t pcm12() const { return (uint8_t)((pulseOut(1) << 4) | pulseOut(0)); }
    uint8_t pcm34() const { return (uint8_t)((noiseOut() << 4) | waveOut()); }

    void tick2() {
        wv.justRead = false;
        if (!power) { accN++; return; }
        for (int k = 0; k < 2; ++k) {
            Pulse& p = sq[k];
            if (!p.on) continue;
            p.timer -= 2;
            if (p.timer <= 0) { p.timer += (2048 - p.freq) * 4; p.pos = (p.pos + 1) & 7; }
        }
        if (wv.on) {
            wv.timer -= 2;
            if (wv.timer <= 0) {
                wv.timer += (2048 - wv.freq) * 2;
                wv.pos = (wv.pos + 1) & 31;
                wv.buf = waveRam[wv.pos >> 1];
                wv.justRead = true;
            }
        }
        if (nz.on) {
            nz.timer -= 2;
            while (nz.timer <= 0) {
                nz.timer += noisePeriod();
                if ((reg[0x12] >> 4) < 14) {
                    uint16_t x = (nz.lfsr ^ (nz.lfsr >> 1)) & 1;
                    nz.lfsr = (uint16_t)((nz.lfsr >> 1) | (x << 14));
                    if (reg[0x12] & 0x08) nz.lfsr = (uint16_t)((nz.lfsr & ~0x40) | (x << 6));
                }
            }
        }
        int ch[4] = {
            sq[0].dac ? pulseOut(0) * 2 - 15 : 0,
            sq[1].dac ? pulseOut(1) * 2 - 15 : 0,
            wv.dac    ? waveOut() * 2 - 15   : 0,
            nz.dac    ? noiseOut() * 2 - 15  : 0
        };
        uint8_t n51 = reg[0x15], n50 = reg[0x14];
        int l = 0, r = 0;
        for (int i = 0; i < 4; ++i) {
            if (n51 & (0x10 << i)) l += ch[i];
            if (n51 & (0x01 << i)) r += ch[i];
        }
        accL += l * (((n50 >> 4) & 7) + 1);
        accR += r * ((n50 & 7) + 1);
        accN++;
    }

    void emitSample() {
        double l = 0, r = 0;
        if (accN > 0) { l = (double)accL / accN * 64.0; r = (double)accR / accN * 64.0; }
        accL = accR = 0; accN = 0;
        double ol = l - capL; capL = l - ol * 0.996;
        double orr = r - capR; capR = r - orr * 0.996;
        auto clip = [](double v) -> int16_t {
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            return (int16_t)v;
        };
        if (sampleCount + 2 <= BUF_SAMPLES * 2) {
            sampleBuf[sampleCount++] = clip(ol);
            sampleBuf[sampleCount++] = clip(orr);
        }
    }

    // dots = T-ciclos a velocidad normal (tiempo real); siempre par.
    void run(int dots) {
        for (int t = 0; t < dots; t += 2) {
            tick2();
            samplePhase += 2LL * SAMPLE_RATE;
            if (samplePhase >= 4194304) { samplePhase -= 4194304; emitSample(); }
        }
    }
};
constexpr uint8_t APU::kReadMask[0x17];
constexpr uint8_t APU::kDuty[4][8];

// ============================================================================
// Bus: cartucho, memoria, timer, PPU, DMA y CPU
// ============================================================================
class Bus {
public:
    static constexpr uint32_t kStateMagic = 0x52455742; // "REWB"
    static constexpr uint32_t kStateVersion = 9;

    // ------------------------------------------------------------ cartucho
    std::vector<uint8_t> rom;
    std::array<uint8_t, 0x20000> ext_ram{};
    bool mbc1Multicart = false;
    uint8_t mbcType = 0;            // 0 nada, 1 MBC1, 2 MBC2, 3 MBC3, 5 MBC5
    bool hasRtc = false, hasBattery = false;
    uint32_t ramSize = 0;
    uint16_t romBank = 1;
    uint8_t ramBank = 0;
    bool ramEnabled = false;
    uint8_t mbcMode = 0;
    uint8_t rtcRegs[5] = {0}, rtcLatched[5] = {0};
    uint8_t rtcLatchPrev = 0xFF;
    uint32_t rtcSubDots = 0;

    // ------------------------------------------------------------ memoria
    std::array<uint8_t, 0x2000> vram{}, vram1{};
    std::array<uint8_t, 0x8000> wram{};
    std::array<uint8_t, 0xA0> oam{};
    std::array<uint8_t, 0x7F> hram{};
    uint8_t ie = 0, iflag = 0xE1;

    // ------------------------------------------------------------ CGB
    bool isCgbMode = false;
    uint8_t vbk = 0, svbk = 0, key1Armed = 0, opri = 0;
    uint8_t ff72 = 0, ff73 = 0, ff74 = 0, ff75 = 0;
    bool cgbDoubleSpeed = false;
    std::array<uint8_t, 64> bgPaletteRam{}, objPaletteRam{};
    uint8_t bcps = 0, ocps = 0;
    uint16_t hdmaSrc = 0, hdmaDst = 0;
    uint8_t hdmaBlocksLeft = 0;
    bool hdmaActive = false;
    uint8_t hdmaStatus = 0xFF;
    int stallDots = 0;

    // ------------------------------------------------------------ OAM DMA
    bool dmaActive = false;
    uint8_t dmaIndex = 0, dmaReg = 0xFF;
    uint16_t dmaSrc = 0, dmaPendingSrc = 0;
    int dmaStartDelay = 0;

    // ------------------------------------------------------------ timer
    uint16_t divCounter = 0;
    uint8_t tima = 0, tma = 0, tac = 0xF8;
    uint8_t timaState = 0;          // 1 = TIMA desbordó (lee 0), 2 = ciclo de recarga
    static constexpr int kClockSelectBit[4] = {9, 3, 5, 7};

    // ------------------------------------------------------------ serie
    uint8_t sb = 0, sc = 0x7E;
    int serialCycles = 0;
    std::string serialOut;

    // ------------------------------------------------------------ joypad
    uint8_t joypSelect = 0x30, buttonsState = 0xFF;
    static constexpr uint8_t kButtonBit[8] = { 2, 3, 1, 0, 4, 5, 6, 7 };

    // ------------------------------------------------------------ SGB (ICD2)
    // El SGB recibe "command packets" por los bits 4/5 de JOYP (P1). Cada
    // pulso empieza con P14=P15=0 y a continuacion cada bit se envia bajando
    // P14 (bit=0) o P15 (bit=1). Los paquetes CHR_TRN ($13) y PCT_TRN ($14)
    // definen el borde 256x224 (tiles 4bpp + tilemap + 3 paletas de 15 colores).
    uint8_t sgbP1Prev = 0x30;
    int sgbPhase = 0;            // 0=espera, 1=recibiendo 16 bytes, 2=fin de paquete
    int sgbByteIndex = 0, sgbBitCount = 0;
    uint8_t sgbBitBuf = 0, sgbPacket[16] = {0};
    bool sgbMultiPlayer = false; // MLT_REQ activo (modo multijugador)
    int sgbPlayerCount = 2, sgbPlayerIndex = 0;
    std::array<uint8_t, 0x2000> sgbTileData{};  // 256 tiles x 32 bytes
    std::array<uint8_t, 0x800> sgbMapData{};    // tilemap 32x28 + fila extra
    std::array<uint16_t, 64> sgbPalData{};      // paletas 0-3, RGB555
    bool sgbHasTiles = false, sgbHasMap = false;
    // Borde decodificado (256x224, RGB888).
    uint32_t sgbDecodedBorder[SGB_W * SGB_H] = {0};
    bool sgbBorderFromPacket = false;

    static uint32_t sgbRgb555to888(uint16_t c) {
        int r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    // Pixel 4bpp: dos "Game Boy tiles" intercalados (bits 0-1 y bits 2-3).
    // El indice de color 0 de cada paleta es transparente (muestra el fondo).
    uint32_t sgbTilePixel(int tile, int px, int py, int pal) const {
        int off = tile * 32 + py * 2;
        uint8_t lo = sgbTileData[off] >> (7 - px);
        uint8_t hi = sgbTileData[off + 1] >> (7 - px);
        uint8_t l2 = sgbTileData[off + 16] >> (7 - px);
        uint8_t h2 = sgbTileData[off + 17] >> (7 - px);
        int idx = (lo & 1) | ((hi & 1) << 1) | ((l2 & 1) << 2) | ((h2 & 1) << 3);
        if (idx == 0) return 0x000000;   // color 0 = transparente (fondo)
        return sgbRgb555to888(sgbPalData[pal * 16 + idx]);
    }
    void sgbRebuildBorderInto(uint32_t* out) {
        if (!sgbHasTiles || !sgbHasMap) return;
        for (int my = 0; my < 28; ++my)
            for (int mx = 0; mx < 32; ++mx) {
                uint16_t e = (uint16_t)(sgbMapData[(my * 32 + mx) * 2] |
                                        (sgbMapData[(my * 32 + mx) * 2 + 1] << 8));
                if (e & 0x300) continue;          // entrada no usada (transparente)
                int tile = e & 0xFF;
                int pal = (e >> 10) & 3;
                bool xf = (e & 0x4000) != 0, yf = (e & 0x8000) != 0;
                for (int py = 0; py < 8; ++py)
                    for (int px = 0; px < 8; ++px) {
                        int dx = mx * 8 + (xf ? 7 - px : px);
                        int dy = my * 8 + (yf ? 7 - py : py);
                        out[dy * SGB_W + dx] = sgbTilePixel(tile, px, py, pal);
                    }
            }
    }
    void sgbRebuildBorder() {
        if (!sgbHasTiles || !sgbHasMap) return;
        sgbRebuildBorderInto(sgbDecodedBorder);
        sgbBorderFromPacket = true;
    }
    // Firma SGB en el registro C tras el arranque: C=0x14 (SGB/SGB2), C=0x00
    // (CGB), C=0x13 (DMG normal). Solo se expone cuando la peticion de modo la
    // activa, de modo que la firma "por defecto" de cada hardware se conserva.
    int sgbSignatureMode = 0;   // 0 = normal, 1 = forzar C=0x14

    // Procesa un paquete completo (16 bytes).
    void sgbHandlePacket(const uint8_t* p) {
        uint8_t cmd = p[0] >> 3;
#ifdef REW_SGB_TEST_HOOKS
        { extern uint8_t g_sgb_pkts[64][16]; extern int g_sgb_cmd_n;
          if (g_sgb_cmd_n < 64) { for (int i=0;i<16;i++) g_sgb_pkts[g_sgb_cmd_n][i]=p[i]; g_sgb_cmd_n++; } }
#endif
        switch (cmd) {
            case 0x11: { // MLT_REQ: activa multijugador para deteccion por P1
                int ctrl = p[1] & 3;
                sgbMultiPlayer = (ctrl & 1) != 0;
                sgbPlayerCount = (ctrl & 2) ? 4 : 2;
                sgbPlayerIndex = 0;
                joypSelect = (joypSelect & 0xCF) | 0x10;
                return;
            }
            case 0x13: { // CHR_TRN: 128 tiles (4 KiB) desde VRAM 0x8800
                int base = (p[1] & 1) ? 128 * 32 : 0;
                for (int i = 0; i < 128 * 32; ++i) sgbTileData[base + i] = vram[0x800 + i];
                sgbHasTiles = true;
                break;
            }
            case 0x14: { // PCT_TRN: tilemap + paletas desde VRAM 0x8800
                for (int i = 0; i < 0x700; ++i) sgbMapData[i] = vram[0x800 + i];
                for (int i = 0; i < 64; ++i)   // 4 paletas x 16 colores RGB555
                    sgbPalData[i] = (uint16_t)(vram[0x1000 + i * 2] | (vram[0x1000 + i * 2 + 1] << 8));
                sgbHasMap = true;
                break;
            }
            default: break;
        }
        sgbRebuildBorder();
    }
    // Alimenta P1 tras escribir en FF00 (protocolo ICD2 del SGB).
    void sgbWriteP1(uint8_t value) {
        uint8_t v = value & 0x30;
        bool startPulse = (v == 0x00) && (sgbP1Prev != 0x00);
        // Un pulso de bit es cualquier bajada a 0x10/0x20 desde una fase de
        // reposo (0x00 tras el start, o 0x30), nunca desde otro pulso.
        // En el protocolo ICD2, P14 LOW transmite un 1 y P15 LOW un 0.
        bool fromIdle = (sgbP1Prev == 0x00 || sgbP1Prev == 0x30);
        bool bitOne = (v == 0x10) && fromIdle;
        bool bitZero = (v == 0x20) && fromIdle;
        sgbP1Prev = v;
        if (startPulse) {
            sgbPhase = 1; sgbByteIndex = 0; sgbBitCount = 0; sgbBitBuf = 0;
            return;
        }
        if (sgbPhase != 1 || (!bitOne && !bitZero)) return;
        if (bitOne) sgbBitBuf |= (uint8_t)(1u << sgbBitCount);
        if (++sgbBitCount == 8) {
            if (sgbByteIndex < 16) sgbPacket[sgbByteIndex] = sgbBitBuf;
            sgbByteIndex++;
            sgbBitCount = 0; sgbBitBuf = 0;
            if (sgbByteIndex == 16) { sgbPhase = 2; sgbHandlePacket(sgbPacket); }
        }
    }

    // ------------------------------------------------------------ PPU
    uint8_t lcdc = 0x91, stat = 0x80, scy = 0, scx = 0, ly = 0, lyReg = 0, lyc = 0;
    uint8_t wy = 0, wx = 0, bgp = 0xFC, obp0 = 0xFF, obp1 = 0xFF;
    int lineDot = 0, mode3End = 252;
    uint8_t ppuMode = 0;
    bool statLine = false, lcdFirstLine = false, wyTriggered = false;
    int windowInternalLine = 0;
    uint32_t frameBuf[GB_WIDTH * GB_HEIGHT] = {0};
    bool frameReady = false;

    // ------------------------------------------------------------ CPU
    uint16_t pc = 0x100, sp = 0xFFFE;
    uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    bool ime = false, imeEnablePending = false, halted = false, haltBugPending = false;
    int64_t frameDots = 0;

    APU apu;

    // Desfase de la fila de OAM que el PPU lee en modo 2 (corrupción DMG)
    int oamBugRowOffset = 0;
    int lcdOnStartDot = 4;
    uint8_t statModeSnap = 0;

    // ================================================================ util
    int dotsPerM() const { return cgbDoubleSpeed ? 2 : 4; }
    void requestInterrupt(uint8_t m) { iflag |= m; }

    static uint32_t rgb555ToArgb(uint16_t col) {
        uint8_t r = col & 0x1F, g = (col >> 5) & 0x1F, bb = (col >> 10) & 0x1F;
        auto ex = [](uint8_t v) { return (uint8_t)((v << 3) | (v >> 2)); };
        return ((uint32_t)ex(r) << 16) | ((uint32_t)ex(g) << 8) | (uint32_t)ex(bb);
    }
    static uint32_t dmgShade(uint8_t i) {
        static const uint32_t t[4] = {0x00E0F8D0, 0x0088C070, 0x00346856, 0x00081820};
        return t[i & 3];
    }

    // ================================================================ carga
    bool loadRom(const std::string& path) {
        std::vector<uint8_t> data = LeerFichero(path.c_str());
        if (data.empty()) return false;
        if (data.size() < 0x150) return false;
        size_t sz = 0x8000;
        while (sz < data.size()) sz <<= 1;
        data.resize(sz, 0xFF);
        rom = std::move(data);

        uint8_t type = rom[0x147];
        hasRtc = false; hasBattery = false;
        switch (type) {
            case 0x01: case 0x02: case 0x03: mbcType = 1; break;
            case 0x05: case 0x06: mbcType = 2; break;
            case 0x0F: case 0x10: mbcType = 3; hasRtc = true; break;
            case 0x11: case 0x12: case 0x13: mbcType = 3; break;
            case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: mbcType = 5; break;
            default: mbcType = 0; break;
        }
        switch (type) {
            case 0x03: case 0x06: case 0x09: case 0x0D: case 0x0F: case 0x10:
            case 0x13: case 0x1B: case 0x1E: hasBattery = true; break;
            default: break;
        }
        static const uint32_t kRamSizes[6] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
        uint8_t rs = rom[0x149];
        ramSize = rs < 6 ? kRamSizes[rs] : 0;
        if (mbcType == 2) ramSize = 0x200;
        if (type == 0x10 || type == 0x13 || type == 0x12) { if (ramSize == 0) ramSize = 0x8000; }

        mbc1Multicart = mbcType == 1 && rom.size() == 0x100000 &&
                        std::equal(rom.begin() + 0x104, rom.begin() + 0x134, rom.begin() + 0x40104);
        // Modo CGB: por cabecera del cartucho, salvo que el modo de sistema lo
        // fuerce. Los modos SGB basados en DMG (Super Game Boy y
        // Game Boy + SGB Border) deben arrancar en modo DMG para que el juego
        // detecte el hardware SGB (firma C=0x14) y envie su borde.
        if (g_model_pref == SYS_DMG || ModeIsSgb(g_model_pref)) isCgbMode = false;
        else if (ModeForcesCgb(g_model_pref)) isCgbMode = true;
        else isCgbMode = (rom[0x143] & 0x80) != 0;
        ext_ram.fill(0xFF);
        reset();
        return true;
    }

    void reset() {
        if (isCgbMode) { a = 0x11; f = 0x80; b = 0x00; c = 0x00; d = 0xFF; e = 0x56; h = 0x00; l = 0x0D; }
        else { a = 0x01; f = 0xB0; b = 0x00; c = 0x13; d = 0x00; e = 0xD8; h = 0x01; l = 0x4D; }
        // Firma de arranque: C=0x14 anuncia un Super Game Boy (SGB/SGB2) y hace
        // que el juego habilite sus funciones SGB (y envie su borde). Los modos
        // DMG+SGB la exponen siempre; los modos CGB+SGB la piden solo durante la
        // captura del borde (la partida real conserva C=0x00 y su color CGB).
        if (sgbSignatureMode || (!isCgbMode && ModeIsSgb(g_model_pref))) c = 0x14;
        pc = 0x100; sp = 0xFFFE;
        ime = imeEnablePending = halted = haltBugPending = false;
        divCounter = isCgbMode ? 0x1EA0 : 0xABCC;
        tima = 0; tma = 0; tac = 0xF8; timaState = 0;
        romBank = 1; ramBank = 0; ramEnabled = (mbcType == 0); mbcMode = 0;
        rtcLatchPrev = 0xFF; rtcSubDots = 0;
        dmaActive = false; dmaIndex = 0; dmaReg = 0xFF; dmaSrc = 0; dmaPendingSrc = 0; dmaStartDelay = 0;
        joypSelect = 0x30; buttonsState = 0xFF;
        sgbP1Prev = 0x30; sgbPhase = 0; sgbByteIndex = 0; sgbBitCount = 0; sgbBitBuf = 0;
        sgbMultiPlayer = false; sgbPlayerCount = 2; sgbPlayerIndex = 0;
        sgbHasTiles = sgbHasMap = false; sgbBorderFromPacket = false;
        sgbTileData.fill(0); sgbMapData.fill(0); sgbPalData.fill(0);
        memset(sgbDecodedBorder, 0, sizeof(sgbDecodedBorder));
        sb = 0; sc = 0x7E; serialCycles = 0; serialOut.clear();
        vram.fill(0); vram1.fill(0); wram.fill(0); oam.fill(0); hram.fill(0);
        ie = 0; iflag = 0xE1;
        lcdc = 0x91; stat = 0x80; scy = scx = 0; lyc = 0; wy = wx = 0;
        bgp = 0xFC; obp0 = obp1 = 0xFF;
        ly = 0; lyReg = 0; lineDot = 0; ppuMode = 0; statLine = false;
        lcdFirstLine = false; wyTriggered = false; windowInternalLine = 0; mode3End = 252;
        frameReady = false; frameDots = 0;
        vbk = 0; svbk = 0; key1Armed = 0; opri = 0; ff72 = ff73 = ff74 = 0; ff75 = 0;
        cgbDoubleSpeed = false; bcps = ocps = 0;
        for (int i = 0; i < 64; i += 2) {
            bgPaletteRam[i] = 0xFF; bgPaletteRam[i + 1] = 0x7F;
            objPaletteRam[i] = 0xFF; objPaletteRam[i + 1] = 0x7F;
        }
        hdmaSrc = hdmaDst = 0; hdmaBlocksLeft = 0; hdmaActive = false; hdmaStatus = 0xFF; stallDots = 0;
        apu.reset(isCgbMode);
        for (auto& px : frameBuf) px = isCgbMode ? 0x00FFFFFF : dmgShade(0);
    }

    // ================================================================ MBC
    int mbc1HighShift() const { return mbc1Multicart ? 4 : 5; }
    uint8_t romRead(uint16_t addr) const {
        size_t banks = rom.size() / 0x4000;
        size_t bank;
        if (addr < 0x4000) {
            bank = (mbcType == 1 && mbcMode) ? (size_t)((ramBank & 3) << mbc1HighShift()) : 0;
        } else {
            switch (mbcType) {
                case 1: bank = (size_t)(((ramBank & 3) << mbc1HighShift()) | (romBank & (mbc1Multicart ? 0x0F : 0x1F))); break;
                case 0: bank = 1; break;
                default: bank = romBank; break;
            }
        }
        bank %= banks;
        return rom[bank * 0x4000 + (addr & 0x3FFF)];
    }

    void mbcWrite(uint16_t addr, uint8_t v) {
        switch (mbcType) {
            case 1:
                if (addr < 0x2000) ramEnabled = (v & 0x0F) == 0x0A;
                else if (addr < 0x4000) { romBank = v & 0x1F; if (romBank == 0) romBank = 1; }
                else if (addr < 0x6000) ramBank = v & 3;
                else mbcMode = v & 1;
                break;
            case 2:
                if (addr < 0x4000) {
                    if (addr & 0x100) { romBank = v & 0x0F; if (romBank == 0) romBank = 1; }
                    else ramEnabled = (v & 0x0F) == 0x0A;
                }
                break;
            case 3:
                if (addr < 0x2000) ramEnabled = (v & 0x0F) == 0x0A;
                else if (addr < 0x4000) {
                    romBank = (rom.size() > 0x200000) ? v : (v & 0x7F);
                    if (romBank == 0) romBank = 1;
                }
                else if (addr < 0x6000) ramBank = v & 0x0F;
                else {
                    if (rtcLatchPrev == 0x00 && v == 0x01) memcpy(rtcLatched, rtcRegs, 5);
                    rtcLatchPrev = v;
                }
                break;
            case 5:
                if (addr < 0x2000) ramEnabled = (v & 0x0F) == 0x0A;
                else if (addr < 0x3000) romBank = (romBank & 0x100) | v;
                else if (addr < 0x4000) romBank = (romBank & 0xFF) | ((v & 1) << 8);
                else if (addr < 0x6000) ramBank = v & 0x0F;
                break;
            default: break;
        }
    }

    uint32_t cartRamOffset(uint16_t addr) const {
        uint32_t bank = (mbcType == 1 && mbcMode == 0) ? 0 : ramBank;
        return (bank * 0x2000 + (addr & 0x1FFF)) % ramSize;
    }

    uint8_t cartRamRead(uint16_t addr) const {
        if (!ramEnabled) return 0xFF;
        if (mbcType == 2) return ext_ram[addr & 0x1FF] | 0xF0;
        if (mbcType == 3 && ramBank >= 0x08) {
            if (!hasRtc || ramBank > 0x0C) return 0xFF;
            static const uint8_t m[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
            int i = ramBank - 8;
            return (rtcLatched[i] & m[i]) | (uint8_t)~m[i];
        }
        if (ramSize == 0) return 0xFF;
        return ext_ram[cartRamOffset(addr)];
    }

    void cartRamWrite(uint16_t addr, uint8_t v) {
        if (!ramEnabled) return;
        if (mbcType == 2) { ext_ram[addr & 0x1FF] = v & 0x0F; return; }
        if (mbcType == 3 && ramBank >= 0x08) {
            if (!hasRtc || ramBank > 0x0C) return;
            static const uint8_t m[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
            int i = ramBank - 8;
            rtcRegs[i] = v & m[i];
            rtcLatched[i] = v & m[i];
            if (i == 0) rtcSubDots = 0;
            return;
        }
        if (ramSize == 0) return;
        ext_ram[cartRamOffset(addr)] = v;
    }

    // ================================================================ RTC
    void rtcIncSecond() {
        uint8_t& s = rtcRegs[0]; uint8_t& m = rtcRegs[1]; uint8_t& hr = rtcRegs[2];
        s = (s + 1) & 0x3F;
        if (s != 60) return;
        s = 0; m = (m + 1) & 0x3F;
        if (m != 60) return;
        m = 0; hr = (hr + 1) & 0x1F;
        if (hr != 24) return;
        hr = 0;
        uint16_t day = (uint16_t)(rtcRegs[3] | ((rtcRegs[4] & 1) << 8));
        day++;
        if (day > 511) { day = 0; rtcRegs[4] |= 0x80; }
        rtcRegs[3] = day & 0xFF;
        rtcRegs[4] = (rtcRegs[4] & 0xFE) | ((day >> 8) & 1);
    }
    void rtcAdvanceSeconds(uint64_t secs) {
        if (!hasRtc || (rtcRegs[4] & 0x40) || secs == 0) return;
        if (rtcRegs[0] < 60 && rtcRegs[1] < 60 && rtcRegs[2] < 24) {
            uint64_t day = rtcRegs[3] | ((rtcRegs[4] & 1) << 8);
            uint64_t total = rtcRegs[0] + rtcRegs[1] * 60ULL + rtcRegs[2] * 3600ULL + day * 86400ULL + secs;
            rtcRegs[0] = total % 60; total /= 60;
            rtcRegs[1] = total % 60; total /= 60;
            rtcRegs[2] = total % 24; total /= 24;
            if (total > 511) rtcRegs[4] |= 0x80;
            total %= 512;
            rtcRegs[3] = total & 0xFF;
            rtcRegs[4] = (rtcRegs[4] & 0xFE) | ((total >> 8) & 1);
        } else {
            if (secs > 100000) secs = 100000;
            while (secs--) rtcIncSecond();
        }
    }
    void rtcTick(int dots) {
        if (!hasRtc || (rtcRegs[4] & 0x40)) return;
        rtcSubDots += dots;
        while (rtcSubDots >= 4194304) { rtcSubDots -= 4194304; rtcIncSecond(); }
    }

    // Formato .sav: RAM + (si hay RTC) bloque de 48 bytes compatible con BGB/VBA
    std::vector<uint8_t> batteryData() const {
        std::vector<uint8_t> out(ext_ram.begin(), ext_ram.begin() + ramSize);
        if (hasRtc) {
            auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((v >> (8 * i)) & 0xFF); };
            for (int i = 0; i < 5; ++i) put32(rtcRegs[i]);
            for (int i = 0; i < 5; ++i) put32(rtcLatched[i]);
            uint64_t t = (uint64_t)time(nullptr);
            for (int i = 0; i < 8; ++i) out.push_back((t >> (8 * i)) & 0xFF);
        }
        return out;
    }
    void loadBatteryData(const std::vector<uint8_t>& in) {
        size_t n = std::min<size_t>(in.size(), ramSize);
        if (n) memcpy(ext_ram.data(), in.data(), n);
        if (hasRtc && in.size() >= (size_t)ramSize + 44) {
            const uint8_t* p = in.data() + ramSize;
            auto get32 = [&](int i) { return (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24); };
            for (int i = 0; i < 5; ++i) rtcRegs[i] = (uint8_t)get32(i * 4);
            for (int i = 0; i < 5; ++i) rtcLatched[i] = (uint8_t)get32(20 + i * 4);
            uint64_t saved = get32(40);
            if (in.size() >= (size_t)ramSize + 48) saved |= (uint64_t)get32(44) << 32;
            uint64_t now = (uint64_t)time(nullptr);
            if (saved && now > saved) rtcAdvanceSeconds(now - saved);
        }
    }

    // ================================================================ joypad
    uint8_t readJoyp() const {
        uint8_t r = (joypSelect & 0x30) | 0xC0;
        uint8_t ln = 0x0F;
        // Con multijugador SGB activo y ambas lineas deseleccionadas, P1
        // devuelve el ID del joypad actual ($xF=1, $xE=2, $xD=3, $xC=4).
        if (sgbMultiPlayer && (joypSelect & 0x30) == 0x30)
            return r | (uint8_t)(0x0F & ~(sgbPlayerIndex & 3));
        if (!(joypSelect & 0x10)) ln &= (buttonsState & 0x0F);
        if (!(joypSelect & 0x20)) ln &= ((buttonsState >> 4) & 0x0F);
        return r | ln;
    }
    void setButton(int idx, bool p) {
        if (idx < 0 || idx > 7) return;
        uint8_t bit = kButtonBit[idx];
        bool wasReleased = ((buttonsState >> bit) & 1) != 0;
        if (p) buttonsState &= ~(1u << bit); else buttonsState |= (1u << bit);
        if (p && wasReleased) requestInterrupt(0x10);
    }
    void releaseAllButtons() { buttonsState = 0xFF; }

    // ================================================================ timer
    bool timerSignal() const {
        return (tac & 0x04) && ((divCounter >> kClockSelectBit[tac & 3]) & 1);
    }
    void incTima() {
        if (tima == 0xFF) { tima = 0; timaState = 1; }
        else tima++;
    }
    void timerTickM() {
        if (timaState == 1) { tima = tma; iflag |= 0x04; timaState = 2; }
        else if (timaState == 2) timaState = 0;
        bool oldSig = timerSignal();
        uint16_t oldDiv = divCounter;
        divCounter = (uint16_t)(divCounter + 4);
        if (oldSig && !timerSignal()) incTima();
        int ab = cgbDoubleSpeed ? 13 : 12;
        if (((oldDiv >> ab) & 1) && !((divCounter >> ab) & 1)) apu.frameSeqClock();
    }
    void writeDiv() {
        bool oldSig = timerSignal();
        int ab = cgbDoubleSpeed ? 13 : 12;
        bool apuBit = (divCounter >> ab) & 1;
        divCounter = 0;
        if (oldSig) incTima();
        if (apuBit) apu.frameSeqClock();
    }
    void writeTac(uint8_t v) {
        bool oldSig = timerSignal();
        tac = v | 0xF8;
        if (oldSig && !timerSignal()) incTima();
    }

    // ================================================================ OAM DMA
    uint8_t dmaRead(uint16_t addr) const {
        if (addr >= 0xE000) addr -= 0x2000;
        if (addr < 0x8000) return romRead(addr);
        if (addr < 0xA000) return (isCgbMode && vbk) ? vram1[addr - 0x8000] : vram[addr - 0x8000];
        if (addr < 0xC000) return cartRamRead(addr);
        return wramRead(addr);
    }
    // Escritura en el ciclo M, ciclo M+1 de preparación, bytes en M+2..M+161.
    // La OAM queda bloqueada mientras dura la copia (y durante un reinicio).
    void dmaTickM() {
        if (dmaStartDelay > 0 && --dmaStartDelay == 0) {
            dmaActive = true; dmaIndex = 0; dmaSrc = dmaPendingSrc;
        }
        if (dmaActive) {
            if (dmaIndex >= 160) dmaActive = false;
            else { oam[dmaIndex] = dmaRead((uint16_t)(dmaSrc + dmaIndex)); ++dmaIndex; }
        }
    }

    // ================================================================ WRAM
    int wramBankIndex() const {
        if (!isCgbMode) return 1;
        int bk = svbk & 7;
        return bk ? bk : 1;
    }
    uint8_t wramRead(uint16_t addr) const {
        addr &= 0x1FFF;
        if (addr < 0x1000) return wram[addr];
        return wram[wramBankIndex() * 0x1000 + (addr & 0x0FFF)];
    }
    void wramWrite(uint16_t addr, uint8_t v) {
        addr &= 0x1FFF;
        if (addr < 0x1000) wram[addr] = v;
        else wram[wramBankIndex() * 0x1000 + (addr & 0x0FFF)] = v;
    }

    // ================================================================ HDMA
    uint8_t hdmaReadSrc(uint16_t addr) const {
        if (addr < 0x8000) return romRead(addr);
        if (addr < 0xA000) return 0xFF;
        if (addr < 0xC000) return cartRamRead(addr);
        if (addr < 0xE000) return wramRead(addr);
        return cartRamRead((uint16_t)(addr - 0x4000));
    }
    void hdmaCopyBlock() {
        auto& bank = vbk ? vram1 : vram;
        for (int i = 0; i < 16; ++i) {
            bank[(hdmaDst + i) & 0x1FFF] = hdmaReadSrc((uint16_t)(hdmaSrc + i));
        }
        hdmaSrc = (uint16_t)(hdmaSrc + 16);
        hdmaDst = (uint16_t)((hdmaDst + 16) & 0x1FF0);
        stallDots += 32;
        if (--hdmaBlocksLeft == 0) { hdmaActive = false; hdmaStatus = 0xFF; }
    }
    void writeHdma5(uint8_t v) {
        if (hdmaActive) {
            if (!(v & 0x80)) {
                hdmaActive = false;
                hdmaStatus = 0x80 | ((hdmaBlocksLeft - 1) & 0x7F);
            }
            return;
        }
        hdmaBlocksLeft = (v & 0x7F) + 1;
        if (v & 0x80) {
            hdmaActive = true;
            hdmaStatus = 0;
            if (!(lcdc & 0x80) || (ppuMode == 0 && ly < 144)) hdmaCopyBlock();
        } else {
            while (hdmaBlocksLeft) {
                auto& bank = vbk ? vram1 : vram;
                for (int i = 0; i < 16; ++i) bank[(hdmaDst + i) & 0x1FFF] = hdmaReadSrc((uint16_t)(hdmaSrc + i));
                hdmaSrc = (uint16_t)(hdmaSrc + 16);
                hdmaDst = (uint16_t)((hdmaDst + 16) & 0x1FF0);
                hdmaBlocksLeft--;
                stallDots += 32;
            }
            hdmaStatus = 0xFF;
        }
    }

    // ================================================================ PPU
    // La CPU ve el modo PPU del inicio de su ciclo M.
    bool vramBlocked() const { return (lcdc & 0x80) && statModeSnap == 3; }
    bool oamBlocked() const { return (lcdc & 0x80) && (statModeSnap == 2 || statModeSnap == 3); }

    // Penalización de modo 3 por objetos: 6 dots por objeto visible, más hasta 5 dots
    // para el primer objeto de cada columna de 8 píxeles (alineada con SCX).
    int spritePenalty() const {
        int idx[10];
        int n = spritesOnLine(ly, idx);
        bool seen[32] = {false};
        int pen = 0;
        for (int k = 0; k < n; ++k) {
            int x = oam[idx[k] * 4 + 1];
            if (x >= 168) continue;
            pen += 6;
            int shifted = x + (scx & 7);
            int col = shifted >> 3;
            if (x == 0) { if (!seen[0]) pen += 5; seen[0] = true; continue; }
            if (seen[col]) continue;
            seen[col] = true;
            pen += std::max(0, 5 - (shifted & 7));
        }
        return pen;
    }

    uint8_t vramBankRead(int bank, uint16_t off) const { return bank ? vram1[off] : vram[off]; }

    int spritesOnLine(int line, int* outIdx) const {
        int hgt = (lcdc & 0x04) ? 16 : 8;
        int n = 0;
        for (int i = 0; i < 40 && n < 10; ++i) {
            int sy = oam[i * 4] - 16;
            if (line >= sy && line < sy + hgt) { if (outIdx) outIdx[n] = i; n++; }
        }
        return n;
    }

    void updateStat() {
        bool coinc = (lyReg == lyc);
        stat = (uint8_t)((stat & 0xF8) | (coinc ? 0x04 : 0) | ppuMode);
        bool line = false;
        if (lcdc & 0x80) {
            line = ((stat & 0x40) && coinc) ||
                   (ppuMode == 0 && (stat & 0x08)) ||
                   (ppuMode == 1 && (stat & 0x10)) ||
                   (ppuMode == 2 && (stat & 0x20)) ||
                   (ly == 144 && lineDot == 0 && (stat & 0x20));
        }
        if (line && !statLine) iflag |= 0x02;
        statLine = line;
    }

    void ppuDot() {
        if (!(lcdc & 0x80)) return;
        if (lineDot == 0) {
            if (ly == 0) { windowInternalLine = 0; wyTriggered = false; }
            if (ly < 144) {
                ppuMode = lcdFirstLine ? 0 : 2;
                if (ly == wy) wyTriggered = true;
            } else if (ly == 144) {
                ppuMode = 1;
                iflag |= 0x01;
                memcpy(g_screenBuffer, frameBuf, sizeof(frameBuf));
                frameReady = true;
            }
        }
        if (ly < 144) {
            if (lineDot == 80) {
                ppuMode = 3;
                lcdFirstLine = false;
                int len = 172 + (scx & 7) + ((lcdc & 0x02) || isCgbMode ? spritePenalty() : 0);
                if (wyTriggered && (lcdc & 0x20) && wx <= 166) len += 6;
                if (len > 289) len = 289;
                mode3End = 80 + len;
            } else if (lineDot == mode3End && ppuMode == 3) {
                renderScanline();
                ppuMode = 0;
                if (hdmaActive) hdmaCopyBlock();
            }
        }
        if (ly == 153 && lineDot == 4) lyReg = 0;
        updateStat();
        if (++lineDot >= 456) {
            lineDot = 0;
            ly = (ly >= 153) ? 0 : (uint8_t)(ly + 1);
            lyReg = ly;
        }
    }

    void writeLcdc(uint8_t v) {
        bool wasOn = (lcdc & 0x80) != 0;
        lcdc = v;
        bool on = (v & 0x80) != 0;
        if (wasOn && !on) {
            ly = 0; lyReg = 0; lineDot = 0; ppuMode = 0; statLine = false;
            stat &= 0xFC;
        } else if (!wasOn && on) {
            ly = 0; lyReg = 0; lineDot = lcdOnStartDot; ppuMode = 0;
            lcdFirstLine = true; wyTriggered = (wy == 0); windowInternalLine = 0;
            updateStat();
        }
    }

    void renderScanline() {
        uint8_t bgIdx[GB_WIDTH];
        uint8_t bgAttrPrio[GB_WIDTH];
        uint32_t* row = &frameBuf[ly * GB_WIDTH];
        bool cgb = isCgbMode;
        bool bgOn = cgb || (lcdc & 0x01);
        bool winOn = (lcdc & 0x20) && wyTriggered && wx <= 166 && (cgb || (lcdc & 0x01));
        bool winUsed = false;
        int winX = (int)wx - 7;
        for (int x = 0; x < GB_WIDTH; ++x) {
            bgIdx[x] = 0; bgAttrPrio[x] = 0;
            if (!bgOn) { row[x] = dmgShade(bgp & 3); continue; }
            bool inWin = winOn && x >= winX;
            int px, py; uint16_t mapBase;
            if (inWin) {
                winUsed = true;
                px = x - winX; py = windowInternalLine;
                mapBase = (lcdc & 0x40) ? 0x1C00 : 0x1800;
            } else {
                px = (x + scx) & 0xFF; py = (ly + scy) & 0xFF;
                mapBase = (lcdc & 0x08) ? 0x1C00 : 0x1800;
            }
            uint16_t mapAddr = (uint16_t)(mapBase + (py / 8) * 32 + (px / 8));
            uint8_t tile = vram[mapAddr];
            uint8_t attr = cgb ? vram1[mapAddr] : 0;
            int tileRow = py & 7, tileCol = px & 7;
            if (attr & 0x40) tileRow = 7 - tileRow;
            if (attr & 0x20) tileCol = 7 - tileCol;
            uint16_t tileAddr = (lcdc & 0x10) ? (uint16_t)(tile * 16) : (uint16_t)(0x1000 + (int8_t)tile * 16);
            tileAddr = (uint16_t)(tileAddr + tileRow * 2);
            int bank = (attr & 0x08) ? 1 : 0;
            uint8_t lo = vramBankRead(bank, tileAddr), hi = vramBankRead(bank, (uint16_t)(tileAddr + 1));
            int bit = 7 - tileCol;
            uint8_t ci = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
            bgIdx[x] = ci;
            bgAttrPrio[x] = (attr & 0x80) ? 1 : 0;
            if (cgb) {
                int pi = (attr & 7) * 8 + ci * 2;
                row[x] = rgb555ToArgb((uint16_t)(bgPaletteRam[pi] | (bgPaletteRam[pi + 1] << 8)));
            } else {
                row[x] = dmgShade((bgp >> (ci * 2)) & 3);
            }
        }
        if (winUsed) windowInternalLine++;

        if (!(lcdc & 0x02)) return;
        int idx[10];
        int n = spritesOnLine(ly, idx);
        int order[10];
        for (int i = 0; i < n; ++i) order[i] = idx[i];
        if (!cgb) {
            std::stable_sort(order, order + n, [&](int p, int q) { return oam[p * 4 + 1] < oam[q * 4 + 1]; });
        }
        int hgt = (lcdc & 0x04) ? 16 : 8;
        bool claimed[GB_WIDTH] = {false};
        for (int k = 0; k < n; ++k) {
            int i = order[k];
            int sy = oam[i * 4] - 16, sx = oam[i * 4 + 1] - 8;
            uint8_t tile = oam[i * 4 + 2], fl = oam[i * 4 + 3];
            if (hgt == 16) tile &= 0xFE;
            int line = ly - sy;
            if (fl & 0x40) line = hgt - 1 - line;
            uint16_t addr = (uint16_t)(tile * 16 + line * 2);
            int bank = (cgb && (fl & 0x08)) ? 1 : 0;
            uint8_t lo = vramBankRead(bank, addr), hi = vramBankRead(bank, (uint16_t)(addr + 1));
            for (int p = 0; p < 8; ++p) {
                int x = sx + p;
                if (x < 0 || x >= GB_WIDTH || claimed[x]) continue;
                int bit = (fl & 0x20) ? p : 7 - p;
                uint8_t ci = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
                if (ci == 0) continue;
                claimed[x] = true;
                bool hide;
                if (cgb) hide = (lcdc & 0x01) && bgIdx[x] != 0 && ((fl & 0x80) || bgAttrPrio[x]);
                else hide = (fl & 0x80) && bgIdx[x] != 0;
                if (hide) continue;
                if (cgb) {
                    int pi = (fl & 7) * 8 + ci * 2;
                    row[x] = rgb555ToArgb((uint16_t)(objPaletteRam[pi] | (objPaletteRam[pi + 1] << 8)));
                } else {
                    uint8_t pal = (fl & 0x10) ? obp1 : obp0;
                    row[x] = dmgShade((pal >> (ci * 2)) & 3);
                }
            }
        }
    }

    // ================================================================ OAM bug (DMG)
    bool oamBugActive() const {
        return !isCgbMode && (lcdc & 0x80) && ppuMode == 2 && !lcdFirstLine && ly < 144;
    }
    int oamBugRow() const { return lineDot / 4 + oamBugRowOffset; }
    uint16_t oamWord(int r, int w) const { return (uint16_t)(oam[r * 8 + w * 2] | (oam[r * 8 + w * 2 + 1] << 8)); }
    void setOamWord(int r, int w, uint16_t v) { oam[r * 8 + w * 2] = v & 0xFF; oam[r * 8 + w * 2 + 1] = v >> 8; }
    void oamCopyRow(int dst, int src) { for (int i = 0; i < 8; ++i) oam[dst * 8 + i] = oam[src * 8 + i]; }
    void oamCorruptWrite() {
        int r = oamBugRow();
        if (r < 1 || r >= 20) return;
        uint16_t aa = oamWord(r, 0), bb = oamWord(r - 1, 0), cc = oamWord(r - 1, 2);
        setOamWord(r, 0, (uint16_t)(((aa ^ cc) & (bb ^ cc)) ^ cc));
        for (int w = 1; w < 4; ++w) setOamWord(r, w, oamWord(r - 1, w));
    }
    void oamCorruptRead() {
        int r = oamBugRow();
        if (r < 1 || r >= 20) return;
        uint16_t aa = oamWord(r, 0), bb = oamWord(r - 1, 0), cc = oamWord(r - 1, 2);
        setOamWord(r, 0, (uint16_t)(bb | (aa & cc)));
        for (int w = 1; w < 4; ++w) setOamWord(r, w, oamWord(r - 1, w));
    }
    void oamCorruptReadInc() {
        int r = oamBugRow();
        if (r >= 4 && r < 19) {
            uint16_t aa = oamWord(r - 2, 0), bb = oamWord(r - 1, 0), cc = oamWord(r, 0), dd = oamWord(r - 1, 2);
            setOamWord(r - 1, 0, (uint16_t)((bb & (aa | cc | dd)) | (aa & cc & dd)));
            oamCopyRow(r, r - 1);
            oamCopyRow(r - 2, r - 1);
        }
    }
    static bool inOamRange(uint16_t v) { return v >= 0xFE00 && v <= 0xFEFF; }

    // ================================================================ bus CPU
    uint8_t ioRead(uint16_t addr) {
        if (addr >= 0xFF10 && addr <= 0xFF3F) return apu.read(addr);
        switch (addr) {
            case 0xFF00: return readJoyp();
            case 0xFF01: return sb;
            case 0xFF02: return sc | (isCgbMode ? 0x7C : 0x7E);
            case 0xFF04: return (uint8_t)(divCounter >> 8);
            case 0xFF05: return tima;
            case 0xFF06: return tma;
            case 0xFF07: return tac | 0xF8;
            case 0xFF0F: return iflag | 0xE0;
            case 0xFF40: return lcdc;
            case 0xFF41: return (lcdc & 0x80) ? ((stat & 0xFC) | statModeSnap | 0x80) : ((stat & 0xFC) | 0x80);
            case 0xFF42: return scy;
            case 0xFF43: return scx;
            case 0xFF44: return (lcdc & 0x80) ? lyReg : 0;
            case 0xFF45: return lyc;
            case 0xFF46: return dmaReg;
            case 0xFF47: return bgp;
            case 0xFF48: return obp0;
            case 0xFF49: return obp1;
            case 0xFF4A: return wy;
            case 0xFF4B: return wx;
            default: break;
        }
        if (!isCgbMode) return 0xFF;
        switch (addr) {
            case 0xFF4D: return (uint8_t)(0x7E | (cgbDoubleSpeed ? 0x80 : 0) | (key1Armed & 1));
            case 0xFF4F: return vbk | 0xFE;
            case 0xFF55: return hdmaActive ? (uint8_t)((hdmaBlocksLeft - 1) & 0x7F) : hdmaStatus;
            case 0xFF68: return bcps | 0x40;
            case 0xFF69: return vramBlocked() ? 0xFF : bgPaletteRam[bcps & 0x3F];
            case 0xFF6A: return ocps | 0x40;
            case 0xFF6B: return vramBlocked() ? 0xFF : objPaletteRam[ocps & 0x3F];
            case 0xFF6C: return opri | 0xFE;
            case 0xFF70: return svbk | 0xF8;
            case 0xFF72: return ff72;
            case 0xFF73: return ff73;
            case 0xFF74: return ff74;
            case 0xFF75: return ff75 | 0x8F;
            case 0xFF76: return apu.pcm12();
            case 0xFF77: return apu.pcm34();
            default: return 0xFF;
        }
    }

    void ioWrite(uint16_t addr, uint8_t v) {
        if (addr >= 0xFF10 && addr <= 0xFF3F) { apu.write(addr, v); return; }
        switch (addr) {
            case 0xFF00: {
                uint8_t prev = joypSelect;
                joypSelect = v & 0x30;
                // P15 LOW->HIGH hace avanzar el joypad seleccionado (SGB).
                if (sgbMultiPlayer && (prev & 0x20) == 0 && (joypSelect & 0x20))
                    sgbPlayerIndex = (sgbPlayerIndex + 1) % sgbPlayerCount;
#ifdef REW_SGB_TEST_HOOKS
                { extern void sgbRawLog(uint8_t); sgbRawLog(v); }
#endif
                sgbWriteP1(v);
                return;
            }
            case 0xFF01: sb = v; return;
            case 0xFF02:
                sc = v;
                if ((v & 0x81) == 0x81) {
                    serialOut.push_back((char)sb);
                    serialCycles = (isCgbMode && (v & 0x02)) ? 32 : 1024;
                }
                return;
            case 0xFF04: writeDiv(); return;
            case 0xFF05:
                if (timaState == 2) return;
                if (timaState == 1) timaState = 0;
                tima = v; return;
            case 0xFF06: tma = v; if (timaState == 2) tima = v; return;
            case 0xFF07: writeTac(v); return;
            case 0xFF0F: iflag = v | 0xE0; return;
            case 0xFF40: writeLcdc(v); return;
            case 0xFF41: {
                if (!isCgbMode && (lcdc & 0x80) && !statLine &&
                    (ppuMode == 0 || ppuMode == 1 || (lyReg == lyc))) iflag |= 0x02;
                stat = (uint8_t)((stat & 0x07) | (v & 0x78));
                updateStat();
                return;
            }
            case 0xFF42: scy = v; return;
            case 0xFF43: scx = v; return;
            case 0xFF44: return;
            case 0xFF45: lyc = v; if (lcdc & 0x80) updateStat(); return;
            case 0xFF46: dmaReg = v; dmaPendingSrc = (uint16_t)(v << 8); dmaStartDelay = 2; return;
            case 0xFF47: bgp = v; return;
            case 0xFF48: obp0 = v; return;
            case 0xFF49: obp1 = v; return;
            case 0xFF4A: wy = v; return;
            case 0xFF4B: wx = v; return;
            default: break;
        }
        if (!isCgbMode) return;
        switch (addr) {
            case 0xFF4D: key1Armed = v & 1; return;
            case 0xFF4F: vbk = v & 1; return;
            case 0xFF51: hdmaSrc = (uint16_t)((hdmaSrc & 0x00F0) | (v << 8)); return;
            case 0xFF52: hdmaSrc = (uint16_t)((hdmaSrc & 0xFF00) | (v & 0xF0)); return;
            case 0xFF53: hdmaDst = (uint16_t)((hdmaDst & 0x00F0) | ((v & 0x1F) << 8)); return;
            case 0xFF54: hdmaDst = (uint16_t)((hdmaDst & 0x1F00) | (v & 0xF0)); return;
            case 0xFF55: writeHdma5(v); return;
            case 0xFF68: bcps = v & 0xBF; return;
            case 0xFF69:
                if (!vramBlocked()) bgPaletteRam[bcps & 0x3F] = v;
                if (bcps & 0x80) bcps = (uint8_t)(0x80 | ((bcps + 1) & 0x3F));
                return;
            case 0xFF6A: ocps = v & 0xBF; return;
            case 0xFF6B:
                if (!vramBlocked()) objPaletteRam[ocps & 0x3F] = v;
                if (ocps & 0x80) ocps = (uint8_t)(0x80 | ((ocps + 1) & 0x3F));
                return;
            case 0xFF6C: opri = v & 1; return;
            case 0xFF70: svbk = v & 7; return;
            case 0xFF72: ff72 = v; return;
            case 0xFF73: ff73 = v; return;
            case 0xFF74: ff74 = v; return;
            case 0xFF75: ff75 = v & 0x70; return;
            default: return;
        }
    }

    uint8_t readByte(uint16_t addr) {
        if (addr < 0x8000) return romRead(addr);
        if (addr < 0xA000) {
            if (vramBlocked()) return 0xFF;
            return (isCgbMode && vbk) ? vram1[addr - 0x8000] : vram[addr - 0x8000];
        }
        if (addr < 0xC000) return cartRamRead(addr);
        if (addr < 0xFE00) return wramRead(addr);
        if (addr < 0xFF00) {
            if (oamBugActive()) oamCorruptRead();
            if (dmaActive || oamBlocked()) return 0xFF;
            if (addr >= 0xFEA0) return 0x00;
            return oam[addr - 0xFE00];
        }
        if (addr < 0xFF80) return ioRead(addr);
        if (addr < 0xFFFF) return hram[addr - 0xFF80];
        return ie;
    }

    void writeByte(uint16_t addr, uint8_t v) {
        if (addr < 0x8000) { mbcWrite(addr, v); return; }
        if (addr < 0xA000) {
            if (vramBlocked()) return;
            if (isCgbMode && vbk) vram1[addr - 0x8000] = v; else vram[addr - 0x8000] = v;
            return;
        }
        if (addr < 0xC000) { cartRamWrite(addr, v); return; }
        if (addr < 0xFE00) { wramWrite(addr, v); return; }
        if (addr < 0xFF00) {
            if (oamBugActive()) oamCorruptWrite();
            if (dmaActive || oamBlocked() || addr >= 0xFEA0) return;
            oam[addr - 0xFE00] = v;
            return;
        }
        if (addr < 0xFF80) { ioWrite(addr, v); return; }
        if (addr < 0xFFFF) { hram[addr - 0xFF80] = v; return; }
        ie = v;
    }

    // ================================================================ reloj
    void tickM() {
        timerTickM();
        if (serialCycles > 0 && --serialCycles == 0) { sb = 0xFF; sc &= 0x7F; iflag |= 0x08; }
        dmaTickM();
        statModeSnap = ppuMode;
        int dots = dotsPerM();
        for (int i = 0; i < dots; ++i) ppuDot();
        apu.run(dots);
        rtcTick(dots);
        frameDots += dots;
    }

    uint8_t rd(uint16_t addr) { tickM(); return readByte(addr); }
    void wr(uint16_t addr, uint8_t v) { tickM(); writeByte(addr, v); }
    void idle() { tickM(); }
    // ciclo interno con incremento/decremento de 16 bits (bug OAM en DMG)
    void idleInc(uint16_t v) { tickM(); if (inOamRange(v) && oamBugActive()) oamCorruptWrite(); }
    uint8_t rdInc(uint16_t addr) {
        tickM();
        if (inOamRange(addr) && oamBugActive()) oamCorruptReadInc();
        return readByte(addr);
    }
    uint8_t fetch8() {
        uint8_t v = rd(pc);
        if (haltBugPending) haltBugPending = false; else pc++;
        return v;
    }
    uint16_t fetch16() { uint8_t lo = fetch8(); uint8_t hi = fetch8(); return (uint16_t)(lo | (hi << 8)); }

    // ================================================================ registros
    uint16_t getBC() const { return (uint16_t)((b << 8) | c); }
    uint16_t getDE() const { return (uint16_t)((d << 8) | e); }
    uint16_t getHL() const { return (uint16_t)((h << 8) | l); }
    uint16_t getAF() const { return (uint16_t)((a << 8) | (f & 0xF0)); }
    void setBC(uint16_t v) { b = v >> 8; c = v & 0xFF; }
    void setDE(uint16_t v) { d = v >> 8; e = v & 0xFF; }
    void setHL(uint16_t v) { h = v >> 8; l = v & 0xFF; }
    void setAF(uint16_t v) { a = v >> 8; f = v & 0xF0; }
    uint16_t getRP(int i) const { switch (i) { case 0: return getBC(); case 1: return getDE(); case 2: return getHL(); default: return sp; } }
    void setRP(int i, uint16_t v) { switch (i) { case 0: setBC(v); break; case 1: setDE(v); break; case 2: setHL(v); break; default: sp = v; } }
    uint16_t getRP2(int i) const { return i == 3 ? getAF() : getRP(i); }
    void setRP2(int i, uint16_t v) { if (i == 3) setAF(v); else setRP(i, v); }

    uint8_t getR(int i) {
        switch (i) { case 0: return b; case 1: return c; case 2: return d; case 3: return e;
                     case 4: return h; case 5: return l; case 6: return rd(getHL()); default: return a; }
    }
    void setR(int i, uint8_t v) {
        switch (i) { case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
                     case 4: h = v; break; case 5: l = v; break; case 6: wr(getHL(), v); break; default: a = v; }
    }
    bool flagZ() const { return f & 0x80; }
    bool flagC() const { return f & 0x10; }
    void setFlags(bool z, bool n, bool hh, bool cy) {
        f = (uint8_t)((z ? 0x80 : 0) | (n ? 0x40 : 0) | (hh ? 0x20 : 0) | (cy ? 0x10 : 0));
    }
    bool cond(int cc) const {
        switch (cc) { case 0: return !flagZ(); case 1: return flagZ(); case 2: return !flagC(); default: return flagC(); }
    }

    void alu(int op, uint8_t v) {
        int r;
        switch (op) {
            case 0: r = a + v; setFlags((r & 0xFF) == 0, false, ((a & 0xF) + (v & 0xF)) > 0xF, r > 0xFF); a = (uint8_t)r; break;
            case 1: { int cy = flagC() ? 1 : 0; r = a + v + cy;
                      setFlags((r & 0xFF) == 0, false, ((a & 0xF) + (v & 0xF) + cy) > 0xF, r > 0xFF); a = (uint8_t)r; break; }
            case 2: r = a - v; setFlags((r & 0xFF) == 0, true, (a & 0xF) < (v & 0xF), r < 0); a = (uint8_t)r; break;
            case 3: { int cy = flagC() ? 1 : 0; r = a - v - cy;
                      setFlags((r & 0xFF) == 0, true, ((a & 0xF) - (v & 0xF) - cy) < 0, r < 0); a = (uint8_t)r; break; }
            case 4: a &= v; setFlags(a == 0, false, true, false); break;
            case 5: a ^= v; setFlags(a == 0, false, false, false); break;
            case 6: a |= v; setFlags(a == 0, false, false, false); break;
            default: r = a - v; setFlags((r & 0xFF) == 0, true, (a & 0xF) < (v & 0xF), r < 0); break;
        }
    }

    void push16(uint16_t v) { sp--; wr(sp, v >> 8); sp--; wr(sp, v & 0xFF); }
    uint16_t pop16() { uint8_t lo = rdInc(sp); sp++; uint8_t hi = rd(sp); sp++; return (uint16_t)(lo | (hi << 8)); }

    uint16_t addSpE() {
        int8_t ev = (int8_t)fetch8();
        uint16_t r = (uint16_t)(sp + ev);
        setFlags(false, false, ((sp & 0xF) + ((uint8_t)ev & 0xF)) > 0xF, ((sp & 0xFF) + (uint8_t)ev) > 0xFF);
        return r;
    }

    void executeCB() {
        uint8_t op = fetch8();
        int r = op & 7, bit = (op >> 3) & 7;
        uint8_t v = getR(r);
        switch (op >> 6) {
            case 0: {
                uint8_t res = 0; bool cy = false;
                switch (bit) {
                    case 0: cy = v & 0x80; res = (uint8_t)((v << 1) | (v >> 7)); break;
                    case 1: cy = v & 1; res = (uint8_t)((v >> 1) | (v << 7)); break;
                    case 2: cy = v & 0x80; res = (uint8_t)((v << 1) | (flagC() ? 1 : 0)); break;
                    case 3: cy = v & 1; res = (uint8_t)((v >> 1) | (flagC() ? 0x80 : 0)); break;
                    case 4: cy = v & 0x80; res = (uint8_t)(v << 1); break;
                    case 5: cy = v & 1; res = (uint8_t)((v >> 1) | (v & 0x80)); break;
                    case 6: cy = false; res = (uint8_t)((v << 4) | (v >> 4)); break;
                    default: cy = v & 1; res = (uint8_t)(v >> 1); break;
                }
                setFlags(res == 0, false, false, cy);
                setR(r, res);
                break;
            }
            case 1: f = (uint8_t)((f & 0x10) | 0x20 | (((v >> bit) & 1) ? 0 : 0x80)); break;
            case 2: setR(r, (uint8_t)(v & ~(1 << bit))); break;
            default: setR(r, (uint8_t)(v | (1 << bit))); break;
        }
    }

    void execute(uint8_t op) {
        if (op >= 0x40 && op < 0x80) {
            if (op == 0x76) {
                if (!ime && (ie & iflag & 0x1F)) {
                    if (imeEnablePending) halted = false;
                    else haltBugPending = true;
                } else halted = true;
                return;
            }
            setR((op >> 3) & 7, getR(op & 7));
            return;
        }
        if (op >= 0x80 && op < 0xC0) { alu((op >> 3) & 7, getR(op & 7)); return; }
        switch (op) {
            case 0x00: return;
            case 0x01: case 0x11: case 0x21: case 0x31: setRP(op >> 4, fetch16()); return;
            case 0x02: wr(getBC(), a); return;
            case 0x12: wr(getDE(), a); return;
            case 0x22: { uint16_t hl = getHL(); wr(hl, a); setHL((uint16_t)(hl + 1)); return; }
            case 0x32: { uint16_t hl = getHL(); wr(hl, a); setHL((uint16_t)(hl - 1)); return; }
            case 0x0A: a = rd(getBC()); return;
            case 0x1A: a = rd(getDE()); return;
            case 0x2A: { uint16_t hl = getHL(); a = rdInc(hl); setHL((uint16_t)(hl + 1)); return; }
            case 0x3A: { uint16_t hl = getHL(); a = rdInc(hl); setHL((uint16_t)(hl - 1)); return; }
            case 0x03: case 0x13: case 0x23: case 0x33: { int i = op >> 4; uint16_t v = getRP(i); idleInc(v); setRP(i, (uint16_t)(v + 1)); return; }
            case 0x0B: case 0x1B: case 0x2B: case 0x3B: { int i = op >> 4; uint16_t v = getRP(i); idleInc(v); setRP(i, (uint16_t)(v - 1)); return; }
            case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
                int r = (op >> 3) & 7; uint8_t v = getR(r); uint8_t res = (uint8_t)(v + 1);
                f = (uint8_t)((f & 0x10) | (res == 0 ? 0x80 : 0) | ((v & 0xF) == 0xF ? 0x20 : 0));
                setR(r, res); return;
            }
            case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
                int r = (op >> 3) & 7; uint8_t v = getR(r); uint8_t res = (uint8_t)(v - 1);
                f = (uint8_t)((f & 0x10) | 0x40 | (res == 0 ? 0x80 : 0) | ((v & 0xF) == 0 ? 0x20 : 0));
                setR(r, res); return;
            }
            case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: {
                uint8_t v = fetch8(); setR((op >> 3) & 7, v); return;
            }
            case 0x07: { bool cy = a & 0x80; a = (uint8_t)((a << 1) | (a >> 7)); setFlags(false, false, false, cy); return; }
            case 0x0F: { bool cy = a & 1; a = (uint8_t)((a >> 1) | (a << 7)); setFlags(false, false, false, cy); return; }
            case 0x17: { bool cy = a & 0x80; a = (uint8_t)((a << 1) | (flagC() ? 1 : 0)); setFlags(false, false, false, cy); return; }
            case 0x1F: { bool cy = a & 1; a = (uint8_t)((a >> 1) | (flagC() ? 0x80 : 0)); setFlags(false, false, false, cy); return; }
            case 0x08: { uint16_t nn = fetch16(); wr(nn, sp & 0xFF); wr((uint16_t)(nn + 1), sp >> 8); return; }
            case 0x09: case 0x19: case 0x29: case 0x39: {
                uint16_t hl = getHL(), v = getRP(op >> 4);
                uint32_t r = hl + v;
                f = (uint8_t)((f & 0x80) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? 0x20 : 0) | (r > 0xFFFF ? 0x10 : 0));
                idle(); setHL((uint16_t)r); return;
            }
            case 0x10: {
                fetch8();
                if (isCgbMode && key1Armed) {
                    cgbDoubleSpeed = !cgbDoubleSpeed;
                    key1Armed = 0;
                    for (int i = 0; i < 2050; ++i) idle();
                    divCounter = 0;
                } else {
                    divCounter = 0;
                }
                return;
            }
            case 0x18: { int8_t ev = (int8_t)fetch8(); idle(); pc = (uint16_t)(pc + ev); return; }
            case 0x20: case 0x28: case 0x30: case 0x38: {
                int8_t ev = (int8_t)fetch8();
                if (cond((op >> 3) & 3)) { idle(); pc = (uint16_t)(pc + ev); }
                return;
            }
            case 0x27: {
                bool n = f & 0x40, hh = f & 0x20, cy = f & 0x10;
                if (!n) {
                    if (cy || a > 0x99) { a = (uint8_t)(a + 0x60); cy = true; }
                    if (hh || (a & 0x0F) > 9) a = (uint8_t)(a + 6);
                } else {
                    if (cy) a = (uint8_t)(a - 0x60);
                    if (hh) a = (uint8_t)(a - 6);
                }
                f = (uint8_t)((a == 0 ? 0x80 : 0) | (n ? 0x40 : 0) | (cy ? 0x10 : 0));
                return;
            }
            case 0x2F: a = (uint8_t)~a; f = (uint8_t)(f | 0x60); return;
            case 0x37: f = (uint8_t)((f & 0x80) | 0x10); return;
            case 0x3F: f = (uint8_t)((f & 0x80) | ((f & 0x10) ? 0 : 0x10)); return;
            case 0xC0: case 0xC8: case 0xD0: case 0xD8:
                idle();
                if (cond((op >> 3) & 3)) { pc = pop16(); idle(); }
                return;
            case 0xC9: pc = pop16(); idle(); return;
            case 0xD9: pc = pop16(); idle(); ime = true; imeEnablePending = false; return;
            case 0xC1: case 0xD1: case 0xE1: case 0xF1: setRP2((op >> 4) & 3, pop16()); return;
            case 0xC5: case 0xD5: case 0xE5: case 0xF5: idleInc(sp); push16(getRP2((op >> 4) & 3)); return;
            case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
                uint16_t nn = fetch16();
                if (cond((op >> 3) & 3)) { idle(); pc = nn; }
                return;
            }
            case 0xC3: { uint16_t nn = fetch16(); idle(); pc = nn; return; }
            case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
                uint16_t nn = fetch16();
                if (cond((op >> 3) & 3)) { idleInc(sp); push16(pc); pc = nn; }
                return;
            }
            case 0xCD: { uint16_t nn = fetch16(); idleInc(sp); push16(pc); pc = nn; return; }
            case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
                alu((op >> 3) & 7, fetch8()); return;
            case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
                idleInc(sp); push16(pc); pc = op & 0x38; return;
            case 0xCB: executeCB(); return;
            case 0xE0: { uint8_t n = fetch8(); wr((uint16_t)(0xFF00 | n), a); return; }
            case 0xF0: { uint8_t n = fetch8(); a = rd((uint16_t)(0xFF00 | n)); return; }
            case 0xE2: wr((uint16_t)(0xFF00 | c), a); return;
            case 0xF2: a = rd((uint16_t)(0xFF00 | c)); return;
            case 0xEA: { uint16_t nn = fetch16(); wr(nn, a); return; }
            case 0xFA: { uint16_t nn = fetch16(); a = rd(nn); return; }
            case 0xE8: { uint16_t r = addSpE(); idle(); idle(); sp = r; return; }
            case 0xF8: { uint16_t r = addSpE(); idle(); setHL(r); return; }
            case 0xF9: idleInc(getHL()); sp = getHL(); return;
            case 0xE9: pc = getHL(); return;
            case 0xF3: ime = false; imeEnablePending = false; return;
            case 0xFB: imeEnablePending = true; return;
            default: return; // opcodes ilegales: se tratan como NOP
        }
    }

    void serviceInterrupt() {
        if (haltBugPending) { haltBugPending = false; pc--; }
        ime = false;
        idle();
        idleInc(sp);
        sp--; wr(sp, pc >> 8);
        uint8_t pend = ie & iflag & 0x1F;
        sp--; wr(sp, pc & 0xFF);
        idle();
        if (!pend) { pc = 0; return; }
        int bit = 0;
        while (!((pend >> bit) & 1)) bit++;
        iflag &= (uint8_t)~(1 << bit);
        pc = (uint16_t)(0x40 + bit * 8);
    }

    // Ejecuta una instrucción (o un ciclo de espera). Devuelve dots consumidos.
    int step() {
        int64_t start = frameDots;
        if (stallDots > 0) {
            tickM();
            stallDots -= dotsPerM();
            if (stallDots < 0) stallDots = 0;
            return (int)(frameDots - start);
        }
        if (halted) {
            tickM();
            if (ie & iflag & 0x1F) halted = false;
            return (int)(frameDots - start);
        }
        if (ime && (ie & iflag & 0x1F)) {
            serviceInterrupt();
            return (int)(frameDots - start);
        }
        bool en = imeEnablePending;
        uint8_t op = fetch8();
        execute(op);
        if (en && imeEnablePending) { ime = true; imeEnablePending = false; }
        return (int)(frameDots - start);
    }

    void runFrame() {
        while (frameDots < 70224) step();
        frameDots -= 70224;
    }

    // ================================================================ save states
    template <class F> void serialize(F& io) {
        io(ext_ram); io(mbcType); io(hasRtc); io(hasBattery); io(ramSize); io(romBank); io(ramBank);
        io(ramEnabled); io(mbcMode); io(rtcRegs); io(rtcLatched); io(rtcLatchPrev); io(rtcSubDots);
        io(vram); io(vram1); io(wram); io(oam); io(hram); io(ie); io(iflag);
        io(isCgbMode); io(vbk); io(svbk); io(key1Armed); io(opri); io(ff72); io(ff73); io(ff74); io(ff75);
        io(cgbDoubleSpeed); io(bgPaletteRam); io(objPaletteRam); io(bcps); io(ocps);
        io(hdmaSrc); io(hdmaDst); io(hdmaBlocksLeft); io(hdmaActive); io(hdmaStatus); io(stallDots);
        io(dmaActive); io(dmaIndex); io(dmaReg); io(dmaSrc); io(dmaPendingSrc); io(dmaStartDelay); io(mbc1Multicart);
        io(divCounter); io(tima); io(tma); io(tac); io(timaState);
        io(sb); io(sc); io(serialCycles); io(joypSelect); io(buttonsState);
        io(sgbP1Prev); io(sgbPhase); io(sgbByteIndex); io(sgbBitCount); io(sgbBitBuf);
        io(sgbMultiPlayer); io(sgbPlayerCount); io(sgbPlayerIndex);
        io(sgbPacket); io(sgbTileData); io(sgbMapData); io(sgbPalData);
        io(sgbHasTiles); io(sgbHasMap); io(sgbBorderFromPacket); io(sgbDecodedBorder);
        io(lcdc); io(stat); io(scy); io(scx); io(ly); io(lyReg); io(lyc); io(wy); io(wx);
        io(bgp); io(obp0); io(obp1); io(lineDot); io(mode3End); io(ppuMode); io(statLine);
        io(lcdFirstLine); io(wyTriggered); io(windowInternalLine); io(frameBuf);
        io(pc); io(sp); io(a); io(f); io(b); io(c); io(d); io(e); io(h); io(l);
        io(ime); io(imeEnablePending); io(halted); io(haltBugPending); io(frameDots);
        io(apu.cgb); io(apu.power); io(apu.reg); io(apu.waveRam); io(apu.sq); io(apu.wv); io(apu.nz);
        io(apu.swEn); io(apu.swNegUsed); io(apu.swTimer); io(apu.swShadow); io(apu.fsStep);
    }
};
constexpr int Bus::kClockSelectBit[4];
constexpr uint8_t Bus::kButtonBit[8];

Bus g_bus;

// Copia el frame 160x144 al centro del borde para presentarlo.
void ComponerFrameSgb() {
    if (!g_sgb_border_active) return;
    for (int y = 0; y < SGB_VH; ++y)
        for (int x = 0; x < SGB_VW; ++x)
            g_sgbBuffer[(SGB_VY + y) * SGB_W + (SGB_VX + x)] = g_bus.frameBuf[y * GB_WIDTH + x];
}

// Multiplicador de punto fijo que espera waveOutSetPlaybackRate: la palabra
// alta es la parte entera y la baja la fraccion (0x8000 = 1/2, 0x10000 = 1.0,
// 0x40000 = 4.0). NO es una frecuencia en Hz: pasar 44100/176400 directamente
// se interpreta como multiplicadores de ~0.67x / 2.69x y baja el tono. Se
// calcula como hz/hzBase para que hz == hzBase de exactamente 1.0.
uint32_t MultiplicadorTasaAudio(uint32_t hz, uint32_t hzBase) {
    if (hzBase == 0) return 0x10000;
    return (uint32_t)(((uint64_t)hz << 16) / hzBase);
}

#ifndef REW_HEADLESS
int g_active_slot = 0;
bool g_emulation_paused = false;
char g_rom_base_name[MAX_PATH] = "State";
char g_rom_full_path[MAX_PATH] = "";
int  g_window_scale = 2;

bool g_audio_enabled = true;

// ---------------------------------------------------------------------------
// Carga de bordes externos. Formato propio ".sgb" (cabecera "SGBR"):
//   0x00  "SGBR"   4 bytes
//   0x04  u32 width   (256)
//   0x08  u32 height  (224)
//   0x0C  width*height u32 RGB888 (0 = transparente)
// Tambien se acepta un BMP de 256x224 sin comprimir (24 bpp).
// ---------------------------------------------------------------------------
static bool CargarBordeSgbExterno(const char* path, uint32_t* out) {
    std::vector<uint8_t> d = LeerFichero(path);
    if (d.size() < 16) return false;
    if (d[0] == 'S' && d[1] == 'G' && d[2] == 'B' && d[3] == 'R') {
        uint32_t w = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
        uint32_t h = d[8] | (d[9] << 8) | (d[10] << 16) | ((uint32_t)d[11] << 24);
        if (w != SGB_W || h != SGB_H || d.size() < 12 + (size_t)w * h * 4) return false;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const uint8_t* p = &d[12 + i * 4];
            out[i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
        return true;
    }
    if (d[0] == 'B' && d[1] == 'M') {   // BMP sin comprimir, 24 bpp
        uint32_t off = d[10] | (d[11] << 8) | (d[12] << 16) | ((uint32_t)d[13] << 24);
        int32_t w = (int32_t)(d[18] | (d[19] << 8) | (d[20] << 16) | ((uint32_t)d[21] << 24));
        int32_t h = (int32_t)(d[22] | (d[23] << 8) | (d[24] << 16) | ((uint32_t)d[25] << 24));
        uint16_t bpp = (uint16_t)(d[28] | (d[29] << 8));
        if (bpp != 24 || w != SGB_W || (h != SGB_H && h != -SGB_H)) return false;
        bool topDown = h < 0; int ah = h < 0 ? -h : h;
        size_t row = ((size_t)w * 3 + 3) & ~3u;
        if (d.size() < off + row * ah) return false;
        for (int y = 0; y < SGB_H; ++y) {
            int sy = topDown ? y : (ah - 1 - y);
            for (int x = 0; x < SGB_W; ++x) {
                const uint8_t* p = &d[off + sy * row + (size_t)x * 3];
                out[y * SGB_W + x] = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            }
        }
        return true;
    }
    return false;
}

// Busca un borde externo: .\<rom>.sgb, .\borders\<rom>.sgb o .\border.sgb.
static bool BuscarBordeSgbExterno(const char* romBase) {
    char p[MAX_PATH];
    if (romBase && romBase[0]) {
        wsprintfA(p, ".\\%s.sgb", romBase);
        if (CargarBordeSgbExterno(p, g_sgbBuffer)) return true;
        wsprintfA(p, ".\\borders\\%s.sgb", romBase);
        if (CargarBordeSgbExterno(p, g_sgbBuffer)) return true;
        wsprintfA(p, ".\\borders\\%s.bmp", romBase);
        if (CargarBordeSgbExterno(p, g_sgbBuffer)) return true;
    }
    if (CargarBordeSgbExterno(".\\border.sgb", g_sgbBuffer)) return true;
    if (CargarBordeSgbExterno(".\\border.bmp", g_sgbBuffer)) return true;
    return false;
}

// Reconstruye el borde activo: prioriza el borde real enviado por la ROM,
// luego un archivo externo, y por ultimo el estilo de reserva del modo.
// Si `Force SGB Border` esta desactivado, los juegos sin borde propio se
// muestran sin borde (comportamiento autentico del SGB).
void InitSgbBorder(int mode) {
    g_sgb_border_active = ModeUsesBorder(mode);
    if (!g_sgb_border_active) return;
    if (g_bus.sgbBorderFromPacket) {
        memcpy(g_sgbBuffer, g_bus.sgbDecodedBorder, sizeof(g_sgbBuffer));
        g_sgb_border_index = -1;   // marca: borde real de la ROM
        return;
    }
    // Modos CGB+SGB: el juego CGB no envia su borde por si mismo, pero se
    // capturo antes ejecutandolo en un modo DMG+SGB. Se usa ese borde real.
    if (g_sgb_captured_count > 0) {
        memcpy(g_sgbBuffer, g_sgb_captured_border, sizeof(g_sgbBuffer));
        g_sgb_border_index = -1;
        return;
    }
    if (BuscarBordeSgbExterno(g_rom_base_name)) { g_sgb_border_index = -2; return; }
    if (!g_force_sgb_border) { g_sgb_border_active = false; g_sgb_border_index = 0; return; }
    int theme = (g_sgb_theme_pref >= 0) ? g_sgb_theme_pref : SgbThemeForMode(mode);
    g_sgb_border_index = theme;
    const SgbTheme& t = kSgbThemes[theme];
    GenerarBordeSgb(theme, t.base, t.accent, t.extra);
}

// ---------------------------------------------------------------------------
// Captura del borde real para los modos CGB+SGB. Un juego CGB arranca con la
// firma C=0x00 y no envia paquetes SGB; si se le forzara la firma C=0x14 en
// modo CGB, muchos juegos (Pokemon Gold/Silver/Crystal) se colgarian. En su
// lugar se ejecuta el cartucho en un modo DMG+SGB desechable, se espera a que
// llegue su borde (CHR_TRN/PCT_TRN) y se guarda; despues se restaura la partida
// CGB con color, que mostrara ese borde. Devuelve true si se capturo.
// ---------------------------------------------------------------------------
static const int kSgbCaptureFrames = 600;

bool CapturarBordeSgbParaCgb() {
    if (g_bus.rom.empty()) return false;

    static Bus capBus;
    capBus = g_bus;                       // mismo cartucho, sin tocar g_bus
    capBus.sgbSignatureMode = 1;          // exponer la firma SGB (C=0x14)
    capBus.isCgbMode = false;             // ejecutar como DMG (hardware SGB)
    capBus.reset();

    bool capturado = false;
    for (int i = 0; i < kSgbCaptureFrames; ++i) {
        capBus.runFrame();
        if (capBus.sgbBorderFromPacket) { capturado = true; break; }
    }
    if (capturado) {
        capBus.sgbRebuildBorderInto(g_sgb_captured_border);
        g_sgb_captured_count = SGB_W * SGB_H;
    }
    capBus.rom.clear();                   // no conservar copia del cartucho
    return capturado;
}

// Prepara el borde para el modo pedido. En los modos DMG+SGB el juego lo envia
// por si mismo al arrancar; en los modos CGB+SGB se captura de antemano.
void PrepararBordeParaModo(int mode) {
    g_sgb_captured_count = 0;
    g_sgb_capture_pending = false;
    if (mode == SYS_GBC_SGB || mode == SYS_GBC_INIT_SGB) {
        if (!g_bus.rom.empty()) {
            if (!CapturarBordeSgbParaCgb()) g_sgb_capture_pending = true;
        } else {
            g_sgb_capture_pending = true;
        }
    }
    InitSgbBorder(mode);
}


bool g_auto_fire_enabled[2] = { false, false };   // 0=A, 1=B
int  g_auto_fire_counter[2]  = { 0, 0 };
const int AUTO_FIRE_FRAMES = 10;  // toggle cada 10 frames (~6 Hz)

// Acciones: 0=UP 1=DOWN 2=LEFT 3=RIGHT 4=A 5=B 6=Select 7=Start
//           8=AutoA 9=AutoB 10=Speedup
#define MAX_ACTIONS 11
DWORD g_bindings[MAX_ACTIONS][4] = {
    { VK_UP,    0, 0, 0 }, { VK_DOWN,  0, 0, 0 },
    { VK_LEFT,  0, 0, 0 }, { VK_RIGHT, 0, 0, 0 },
    { 'Z',      0, 0, 0 }, { 'X',      0, 0, 0 },
    { VK_SPACE, 0, 0, 0 }, { VK_RETURN,0, 0, 0 },
    { 0,        0, 0, 0 }, { 0,        0, 0, 0 },
    { VK_TAB,   0, 0, 0 }
};
const char* g_action_names[MAX_ACTIONS] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "Select", "Start",
    "AutoA", "AutoB", "Speedup"
};

WNDPROC g_origEditProc[MAX_ACTIONS][4] = { nullptr };

// Comandos creados en tiempo de ejecución (el .rc no los incluye)
#define ID_SYSTEM_AUTO   40040
#define ID_SYSTEM_DMG    40041
#define ID_AUDIO_TOGGLE  40050
#define ID_SGB_FORCE     40051
#define ID_SGB_THEME_AUTO 40052
#define ID_SGB_THEME_BASE 40060   // + indice de estilo (SGB_THEME_COUNT)

void ActualizarMenuDeEstados(HWND hWnd);

// ============================================================================
// [v0.4] waveOut integration
// ============================================================================
static const int WAVE_HDRS = 32;
static const int WAVE_BUF_SAMPLES = 2048;              // samples por canal
static const int WAVE_BUF_INTS = WAVE_BUF_SAMPLES * 2; // L+R interleaved
static WAVEHDR g_waveHdrs[WAVE_HDRS];
static int16_t g_waveBufs[WAVE_HDRS][WAVE_BUF_INTS];
static int g_waveHdrIdx = 0;
static HWAVEOUT g_hWaveOut = NULL;
static bool g_waveOutReady = false;

void IniciarAudio(HWND hWnd) {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = APU::SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = APU::SAMPLE_RATE * 4;

    if (waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_hWaveOut = NULL;
        g_waveOutReady = false;
        return;
    }

    for (int i = 0; i < WAVE_HDRS; ++i) {
        ZeroMemory(&g_waveHdrs[i], sizeof(WAVEHDR));
        g_waveHdrs[i].lpData = (LPSTR)g_waveBufs[i];
        g_waveHdrs[i].dwBufferLength = WAVE_BUF_INTS * 2;   // bytes
        g_waveHdrs[i].dwFlags = 0;
        waveOutPrepareHeader(g_hWaveOut, &g_waveHdrs[i], sizeof(WAVEHDR));
        g_waveHdrs[i].dwFlags |= WHDR_DONE;
    }
    g_waveOutReady = true;
}

// Ajusta la velocidad de reproduccion de waveOut para que el audio siga a la
// emulacion durante el speedup. Devuelve false si el dispositivo no admite el
// cambio (el llamador silencia entonces el audio durante el speedup).
bool AplicarTasaSpeedup(bool activo) {
    if (!g_waveOutReady || !g_hWaveOut) return false;
    if (!g_speedup_rate_supported) return false;
    // waveOutSetPlaybackRate usa un multiplicador de punto fijo (1.0 = 0x10000),
    // no una frecuencia: pasarle Hz directos bajaba el tono a velocidad normal.
    DWORD target = activo ? MultiplicadorTasaAudio(APU::SAMPLE_RATE * SGB_SPEEDUP_MULT, APU::SAMPLE_RATE)
                          : MultiplicadorTasaAudio(APU::SAMPLE_RATE, APU::SAMPLE_RATE);
    DWORD cur = 0;
    if (waveOutGetPlaybackRate(g_hWaveOut, &cur) == MMSYSERR_NOERROR && cur == target) {
        g_speedup_rate_active = activo;
        return true;
    }
    if (waveOutSetPlaybackRate(g_hWaveOut, target) == MMSYSERR_NOERROR) {
        g_speedup_rate_active = activo;
        return true;
    }
    g_speedup_rate_supported = false;
    return false;
}

void CerrarAudio() {
    if (g_hWaveOut) {
        waveOutReset(g_hWaveOut);
        // Unprepare cada header (buena practica antes de Close)
        for (int i = 0; i < WAVE_HDRS; ++i) {
            if (g_waveHdrs[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(g_hWaveOut, &g_waveHdrs[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(g_hWaveOut);
        g_hWaveOut = NULL;
    }
    g_waveOutReady = false;
}

void EnviarFrameAudio() {
    if (!g_waveOutReady || !g_audio_enabled || g_speedup_audio_muted) { g_bus.apu.sampleCount = 0; return; }

    int n = g_bus.apu.sampleCount;   // samples estéreo totales (L+R ya contados)
    if (n <= 0) return;
    if (n > WAVE_BUF_INTS) n = WAVE_BUF_INTS;

    WAVEHDR* hdr = NULL;
    int idx = -1;
    for (int i = 0; i < WAVE_HDRS; ++i) {
        int k = (g_waveHdrIdx + i) % WAVE_HDRS;
        if (g_waveHdrs[k].dwFlags & WHDR_DONE) {
            hdr = &g_waveHdrs[k];
            idx = k;
            break;
        }
    }
    if (!hdr) {
        g_bus.apu.sampleCount = 0;
        return;
    }

    // Durante el speedup la emulacion genera audio M veces mas rapido que la
    // reproduccion; si aun quedan trozos por sonar, se descarta el actual para
    // no acumular retardo (la reproduccion va a la frecuencia acelerada).
    if (g_speedup_held && g_speedup_rate_active) {
        int pendientes = 0;
        for (int i = 0; i < WAVE_HDRS; ++i)
            if (!(g_waveHdrs[i].dwFlags & WHDR_DONE)) pendientes++;
        if (pendientes > 2) { g_bus.apu.sampleCount = 0; return; }
    }

    memcpy(g_waveBufs[idx], g_bus.apu.sampleBuf, n * sizeof(int16_t));

    hdr->dwBufferLength = n * sizeof(int16_t);
    hdr->dwFlags = WHDR_PREPARED;
    hdr->dwLoops = 0;

    waveOutWrite(g_hWaveOut, hdr, sizeof(WAVEHDR));

    g_waveHdrIdx = (idx + 1) % WAVE_HDRS;
    g_bus.apu.sampleCount = 0;
}

// ============================================================================
// Utilities (INI, SRAM, states, dialog, etc.)
// ============================================================================
void GetIniPath(char* szOut) {
    GetModuleFileNameA(NULL, szOut, MAX_PATH);
    char* p = szOut + lstrlenA(szOut);
    while (p > szOut && *p != '\\') p--;
    if (p > szOut) *(p + 1) = '\0';
    lstrcatA(szOut, "rew.ini");
}

void GuardarConfiguracionIni() {
    char szPath[MAX_PATH]; GetIniPath(szPath);
    char szVal[32];
    for (int a = 0; a < MAX_ACTIONS; ++a)
        for (int c = 0; c < 4; ++c) {
            char key[32];
            wsprintfA(key, "%s_%d", g_action_names[a], c + 1);
            wsprintfA(szVal, "%u", g_bindings[a][c]);
            WritePrivateProfileStringA("Keyboard", key, szVal, szPath);
        }
    wsprintfA(szVal, "%d", g_window_scale);
    WritePrivateProfileStringA("Video", "Scale", szVal, szPath);
    wsprintfA(szVal, "%d", g_audio_enabled ? 1 : 0);
    WritePrivateProfileStringA("Audio", "Enabled", szVal, szPath);
    wsprintfA(szVal, "%d", g_model_pref);
    WritePrivateProfileStringA("System", "Model", szVal, szPath);
    wsprintfA(szVal, "%d", g_force_sgb_border ? 1 : 0);
    WritePrivateProfileStringA("System", "ForceSgbBorder", szVal, szPath);
    wsprintfA(szVal, "%d", g_sgb_theme_pref);
    WritePrivateProfileStringA("System", "BorderStyle", szVal, szPath);
}

void CargarConfiguracionIni() {
    char szPath[MAX_PATH]; GetIniPath(szPath);
    for (int a = 0; a < MAX_ACTIONS; ++a)
        for (int c = 0; c < 4; ++c) {
            char key[32];
            wsprintfA(key, "%s_%d", g_action_names[a], c + 1);
            g_bindings[a][c] = GetPrivateProfileIntA("Keyboard", key, g_bindings[a][c], szPath);
        }
    g_window_scale = GetPrivateProfileIntA("Video", "Scale", 2, szPath);
    if (g_window_scale < 1) g_window_scale = 1;
    if (g_window_scale > 4) g_window_scale = 4;
    g_audio_enabled = GetPrivateProfileIntA("Audio", "Enabled", 1, szPath) != 0;
    g_model_pref = GetPrivateProfileIntA("System", "Model", 0, szPath);
    if (g_model_pref < 0 || g_model_pref >= SYSTEM_MODE_COUNT) g_model_pref = SYS_AUTO;
    g_force_sgb_border = GetPrivateProfileIntA("System", "ForceSgbBorder", 0, szPath) != 0;
    g_sgb_theme_pref = GetPrivateProfileIntA("System", "BorderStyle", -1, szPath);
    if (g_sgb_theme_pref < -1 || g_sgb_theme_pref >= SGB_THEME_COUNT) g_sgb_theme_pref = -1;
    InitSgbBorder(g_model_pref);
}

void CrearMenusExtra(HWND hWnd) {
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;
    HMENU hOption = GetSubMenu(hMenu, 2);
    if (!hOption) return;
    HMENU hSystem = CreatePopupMenu();
    for (int m = 0; m < SYSTEM_MODE_COUNT; ++m)
        AppendMenuA(hSystem, MF_STRING, ID_SYSTEM_AUTO + m, g_system_mode_names[m]);
    AppendMenuA(hOption, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hOption, MF_POPUP, (UINT_PTR)hSystem, "&System");
    AppendMenuA(hOption, MF_STRING, ID_AUDIO_TOGGLE, "&Audio");
    AppendMenuA(hOption, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hOption, MF_STRING, ID_SGB_FORCE, "Force SGB &Border");
    HMENU hTheme = CreatePopupMenu();
    AppendMenuA(hTheme, MF_STRING, ID_SGB_THEME_AUTO, "&Automatico (segun el modo)");
    AppendMenuA(hTheme, MF_SEPARATOR, 0, NULL);
    for (int t = 0; t < SGB_THEME_COUNT; ++t)
        AppendMenuA(hTheme, MF_STRING, ID_SGB_THEME_BASE + t, kSgbThemes[t].name);
    AppendMenuA(hOption, MF_POPUP, (UINT_PTR)hTheme, "Border &Style");
    CheckMenuRadioItem(hMenu, ID_SYSTEM_AUTO, ID_SYSTEM_AUTO + SYSTEM_MODE_COUNT - 1,
                       ID_SYSTEM_AUTO + g_model_pref, MF_BYCOMMAND);
    CheckMenuItem(hMenu, ID_AUDIO_TOGGLE, g_audio_enabled ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hMenu, ID_SGB_FORCE, g_force_sgb_border ? MF_CHECKED : MF_UNCHECKED);
    if (g_sgb_theme_pref < 0)
        CheckMenuRadioItem(hMenu, ID_SGB_THEME_AUTO, ID_SGB_THEME_BASE + SGB_THEME_COUNT - 1,
                           ID_SGB_THEME_AUTO, MF_BYCOMMAND);
    else
        CheckMenuRadioItem(hMenu, ID_SGB_THEME_AUTO, ID_SGB_THEME_BASE + SGB_THEME_COUNT - 1,
                           ID_SGB_THEME_BASE + g_sgb_theme_pref, MF_BYCOMMAND);
    DrawMenuBar(hWnd);
}

void GuardarSramCartucho() {
    if (!g_rom_loaded || !g_bus.hasBattery) return;
    if (g_bus.ramSize == 0 && !g_bus.hasRtc) return;
    CreateDirectoryA(".\\gbsave", NULL);
    char szPath[MAX_PATH];
    wsprintfA(szPath, ".\\gbsave\\%s.sav", g_rom_base_name);
    FOut f(szPath);
    if (!f.ok()) return;
    std::vector<uint8_t> data = g_bus.batteryData();
    f.write(data.data(), data.size());
}

void CargarSramCartucho() {
    if (!g_rom_loaded) return;
    char szPath[MAX_PATH];
    wsprintfA(szPath, ".\\gbsave\\%s.sav", g_rom_base_name);
    std::vector<uint8_t> data = LeerFichero(szPath);
    if (data.empty()) return;   // no existe: juego nuevo
    g_bus.loadBatteryData(data);
}

void GetKeyNameStr(DWORD vk, char* outName) {
    if (vk == 0) { lstrcpyA(outName, "???"); return; }
    if (vk == VK_SPACE) lstrcpyA(outName, "Space");
    else if (vk == VK_RETURN) lstrcpyA(outName, "Enter");
    else if (vk == VK_TAB) lstrcpyA(outName, "Tab");
    else if (vk == VK_BACK) lstrcpyA(outName, "Backspace");
    else if (vk == VK_UP) lstrcpyA(outName, "Up");
    else if (vk == VK_DOWN) lstrcpyA(outName, "Down");
    else if (vk == VK_LEFT) lstrcpyA(outName, "Left");
    else if (vk == VK_RIGHT) lstrcpyA(outName, "Right");
    else if (vk >= 0x30 && vk <= 0x5A) { outName[0] = (char)vk; outName[1] = 0; }
    else if (vk >= VK_F1 && vk <= VK_F12) { wsprintfA(outName, "F%d", vk - VK_F1 + 1); }
    else { wsprintfA(outName, "0x%02X", vk); }
}

// Tabla global para mapear ID de control -> (accion, columna)
static const int kAllCtrlIds[MAX_ACTIONS][4] = {
    { IDC_KEY_UP_1,      IDC_KEY_UP_2,      IDC_KEY_UP_3,      IDC_KEY_UP_4      },
    { IDC_KEY_DOWN_1,    IDC_KEY_DOWN_2,    IDC_KEY_DOWN_3,    IDC_KEY_DOWN_4    },
    { IDC_KEY_LEFT_1,    IDC_KEY_LEFT_2,    IDC_KEY_LEFT_3,    IDC_KEY_LEFT_4    },
    { IDC_KEY_RIGHT_1,   IDC_KEY_RIGHT_2,   IDC_KEY_RIGHT_3,   IDC_KEY_RIGHT_4   },
    { IDC_KEY_A_1,       IDC_KEY_A_2,       IDC_KEY_A_3,       IDC_KEY_A_4       },
    { IDC_KEY_B_1,       IDC_KEY_B_2,       IDC_KEY_B_3,       IDC_KEY_B_4       },
    { IDC_KEY_SELECT_1,  IDC_KEY_SELECT_2,  IDC_KEY_SELECT_3,  IDC_KEY_SELECT_4  },
    { IDC_KEY_START_1,   IDC_KEY_START_2,   IDC_KEY_START_3,   IDC_KEY_START_4   },
    { IDC_KEY_AUTOA_1,   IDC_KEY_AUTOA_2,   IDC_KEY_AUTOA_3,   IDC_KEY_AUTOA_4   },
    { IDC_KEY_AUTOB_1,   IDC_KEY_AUTOB_2,   IDC_KEY_AUTOB_3,   IDC_KEY_AUTOB_4   },
    { IDC_KEY_SPEEDUP_1, IDC_KEY_SPEEDUP_2, IDC_KEY_SPEEDUP_3, IDC_KEY_SPEEDUP_4 }
};

static bool BuscarIndiceBinding(int ctrlId, int& outA, int& outC) {
    for (int a = 0; a < MAX_ACTIONS; ++a)
        for (int c = 0; c < 4; ++c)
            if (kAllCtrlIds[a][c] == ctrlId) { outA = a; outC = c; return true; }
    return false;
}

// Get the 4 column IDs for a given action
int GetColumnID(int action, int col) {
    if (action < 0 || action >= MAX_ACTIONS || col < 0 || col >= 4) return 0;
    return kAllCtrlIds[action][col];
}

LRESULT CALLBACK EditWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Decirle al dialog manager que queremos manejar TODAS las teclas
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }

    int ctrlId = GetDlgCtrlID(hWnd);
    int a = -1, c = -1;
    BuscarIndiceBinding(ctrlId, a, c);

    // Bloquear WM_CHAR (mata el "espacio" de SPACE)
    if (msg == WM_CHAR || msg == WM_SYSCHAR) {
        return 0;
    }

    // Capturar teclas
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        // TAB: capturar como binding (permite mapear Speedup a Tab). El dialogo
        // no navega con Tab para no perder la tecla al configurar.
        // ESC: limpiar el binding
        if (wp == VK_ESCAPE) {
            SetWindowTextA(hWnd, "???");
            if (a >= 0 && c >= 0) g_bindings[a][c] = 0;
            return 0;
        }
        // Cualquier otra tecla: capturar como binding
        char szKey[64] = {0};
        GetKeyNameTextA((LONG)lp, szKey, sizeof(szKey));
        if (szKey[0] == '\0') {
            switch (wp) {
                case VK_SPACE:  lstrcpyA(szKey, "Space");    break;
                case VK_TAB:    lstrcpyA(szKey, "Tab");      break;
                case VK_RETURN: lstrcpyA(szKey, "Enter");    break;
                case VK_BACK:   lstrcpyA(szKey, "Backspace");break;
                case VK_DELETE: lstrcpyA(szKey, "Delete");   break;
                default:        wsprintfA(szKey, "VK%02X", (unsigned)wp); break;
            }
        }
        SetWindowTextA(hWnd, szKey);
        if (a >= 0 && c >= 0) g_bindings[a][c] = (DWORD)wp;
        return 0;
    }

    // Todo lo demás va al WndProc original del Edit
    if (a >= 0 && c >= 0 && g_origEditProc[a][c]) {
        return CallWindowProcA(g_origEditProc[a][c], hWnd, msg, wp, lp);
    }
    return DefWindowProcA(hWnd, msg, wp, lp);
}

INT_PTR CALLBACK KeySettingProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        SendDlgItemMessageA(hDlg, IDC_PLAYER_COMBO, CB_ADDSTRING, 0, (LPARAM)"1P");
        SendDlgItemMessageA(hDlg, IDC_PLAYER_COMBO, CB_SETCURSEL, 0, 0);

        for (int a = 0; a < MAX_ACTIONS; ++a) {
            for (int c = 0; c < 4; ++c) {
                HWND hE = GetDlgItem(hDlg, kAllCtrlIds[a][c]);
                if (!hE) continue;
                char nm[32] = {0};
                GetKeyNameStr(g_bindings[a][c], nm);
                SetWindowTextA(hE, nm);
                // Método clásico de subclasificación: cambiar el WndProc
                g_origEditProc[a][c] = (WNDPROC)SetWindowLongPtrA(
                    hE, GWLP_WNDPROC, (LONG_PTR)EditWndProc);
            }
        }
        HWND hFirst = GetDlgItem(hDlg, IDC_KEY_UP_1);
        if (hFirst) SetFocus(hFirst);
        return FALSE;
    }

    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            // Restaurar WndProcs originales
            for (int a = 0; a < MAX_ACTIONS; ++a)
                for (int c = 0; c < 4; ++c) {
                    HWND hE = GetDlgItem(hDlg, kAllCtrlIds[a][c]);
                    if (hE && g_origEditProc[a][c]) {
                        SetWindowLongPtrA(hE, GWLP_WNDPROC, (LONG_PTR)g_origEditProc[a][c]);
                        g_origEditProc[a][c] = nullptr;
                    }
                }
            GuardarConfiguracionIni();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            // Restaurar WndProcs originales
            for (int a = 0; a < MAX_ACTIONS; ++a)
                for (int c = 0; c < 4; ++c) {
                    HWND hE = GetDlgItem(hDlg, kAllCtrlIds[a][c]);
                    if (hE && g_origEditProc[a][c]) {
                        SetWindowLongPtrA(hE, GWLP_WNDPROC, (LONG_PTR)g_origEditProc[a][c]);
                        g_origEditProc[a][c] = nullptr;
                    }
                }
            CargarConfiguracionIni();  // deshacer cambios no guardados
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
    }

    return FALSE;
}

void ResizeEmulatorWindow(HWND hWnd, int scale) {
    int w = g_sgb_border_active ? SGB_W : GB_WIDTH;
    int h = g_sgb_border_active ? SGB_H : GB_HEIGHT;
    RECT rc = {0, 0, w * scale, h * scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hWnd, NULL, TRUE);
    UpdateWindow(hWnd);
}

void ActualizarEstadoMenusArchivo(HWND hWnd) {
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;
    HMENU hFileMenu = GetSubMenu(hMenu, 0);
    if (!hFileMenu) return;
    UINT estado = g_rom_loaded ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(hFileMenu, ID_FILE_FREEROM, MF_BYCOMMAND | estado);
    EnableMenuItem(hFileMenu, ID_FILE_RESET, MF_BYCOMMAND | estado);
    EnableMenuItem(hFileMenu, ID_FILE_PAUSE, MF_BYCOMMAND | estado);
    if (g_rom_loaded && g_emulation_paused)
        ModifyMenuA(hFileMenu, ID_FILE_PAUSE, MF_BYCOMMAND | MF_STRING, ID_FILE_PAUSE, "Resume");
    else
        ModifyMenuA(hFileMenu, ID_FILE_PAUSE, MF_BYCOMMAND | MF_STRING, ID_FILE_PAUSE, "Pause");
    DrawMenuBar(hWnd);
}

void ActualizarTituloVentana(HWND hWnd);

void LiberarRomActual(HWND hWnd) {
    if (!g_rom_loaded) return;
    GuardarSramCartucho();
    g_bus.rom.clear();
    g_rom_loaded = false;
    g_emulation_paused = false;
    g_rom_display_name[0] = '\0';
    g_sgb_captured_count = 0;
    g_sgb_capture_pending = false;
    for (int i = 0; i < GB_WIDTH * GB_HEIGHT; ++i) g_screenBuffer[i] = 0;
    ActualizarEstadoMenusArchivo(hWnd);
    ActualizarTituloVentana(hWnd);
    InvalidateRect(hWnd, NULL, TRUE);
    UpdateWindow(hWnd);
    MessageBoxA(hWnd, "ROM liberada de la memoria RAM con exito.", "Rew. Reborn", MB_OK | MB_ICONINFORMATION);
}

void ResetearEmulador(HWND hWnd) {
    if (!g_rom_loaded) return;
    // El reset no toca ext_ram: ahí vive el save del cartucho.
    g_bus.reset();
    memcpy(g_screenBuffer, g_bus.frameBuf, sizeof(g_screenBuffer));
    g_emulation_paused = false;
    ActualizarEstadoMenusArchivo(hWnd);
    InvalidateRect(hWnd, NULL, FALSE);
    UpdateWindow(hWnd);
}

void AlternarPausaEmulador(HWND hWnd) {
    if (!g_rom_loaded) return;
    g_emulation_paused = !g_emulation_paused;
    ActualizarEstadoMenusArchivo(hWnd);
}

// Titulo de la ventana: "base - <ROM> - FPS". Se refresca al cargar la ROM y
// tambien desde el bucle principal (contador de FPS).
void ActualizarTituloVentana(HWND hWnd) {
    char titulo[MAX_PATH * 2];
    if (g_rom_display_name[0])
        wsprintfA(titulo, "%s - %s", g_window_base_title, g_rom_display_name);
    else
        lstrcpyA(titulo, g_window_base_title);
    SetWindowTextA(hWnd, titulo);
}

// Nombre del cartucho para el titulo: el titulo de la cabecera de la ROM
// (0x134..0x143) si es legible; si no, el nombre del archivo sin extension.
void EstablecerNombreRom(const char* szFile) {
    const uint8_t* h = g_bus.rom.data();
    size_t n = g_bus.rom.size();
    char nombre[17];
    int k = 0;
    if (n > 0x143) {
        for (int i = 0x134; i <= 0x143; ++i) {
            uint8_t c = h[i];
            if (c == 0) break;
            nombre[k++] = (c >= 32 && c < 127) ? (char)c : ' ';
        }
    }
    nombre[k] = '\0';
    while (k > 0 && nombre[k - 1] == ' ') nombre[--k] = '\0';
    if (k > 0) { lstrcpyA(g_rom_display_name, nombre); return; }
    const char* base = strrchr(szFile, '\\');
    base = base ? base + 1 : szFile;
    lstrcpynA(g_rom_display_name, base, MAX_PATH);
    char* dot = strrchr(g_rom_display_name, '.');
    if (dot) *dot = '\0';
}

bool CargarRomDesdeRuta(HWND hWnd, const char* szFile) {
    if (g_rom_loaded) GuardarSramCartucho();
    if (!g_bus.loadRom(std::string(szFile))) {
        g_rom_loaded = false;
        MessageBoxA(hWnd, "No se pudo cargar la ROM.", "Rew. Reborn", MB_OK | MB_ICONERROR);
        ActualizarEstadoMenusArchivo(hWnd);
        return false;
    }
    lstrcpyA(g_rom_full_path, szFile);
    g_rom_loaded = true;
    const char* file_name = strrchr(szFile, '\\');
    if (file_name) file_name++;
    else file_name = szFile;
    lstrcpyA(g_rom_base_name, file_name);
    char* dot = strrchr(g_rom_base_name, '.');
    if (dot) *dot = '\0';
    CargarSramCartucho();
    // Nombre del cartucho para el titulo de la ventana.
    EstablecerNombreRom(szFile);
    // El borde real de la ROM se recibe por paquetes SGB al arrancar; hay que
    // reencuadrar la ventana cuando llegue (se detecta en el bucle principal).
    g_sgb_border_checked = false;
    PrepararBordeParaModo(g_model_pref);
    ResizeEmulatorWindow(hWnd, g_window_scale);
    memcpy(g_screenBuffer, g_bus.frameBuf, sizeof(g_screenBuffer));
    g_emulation_paused = false;
    ActualizarEstadoMenusArchivo(hWnd);
    ActualizarTituloVentana(hWnd);
    RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    return true;
}

void OpenRomDialog(HWND hWnd) {
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Game Boy ROMs\0*.gb;*.gbc;*.cgb\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) CargarRomDesdeRuta(hWnd, szFile);
}

void ActualizarMenuDeEstados(HWND hWnd) {
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;
    HMENU hStateMenu = GetSubMenu(hMenu, 1);
    if (!hStateMenu) return;
    char szPath[MAX_PATH];
    char szMenuText[80];
    for (int i = 0; i < 10; ++i) {
        wsprintfA(szPath, ".\\gbsave\\%s.st%d", g_rom_base_name, i);
        HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            FILETIME ftWrite; SYSTEMTIME stUTC, stLocal;
            if (GetFileTime(hFile, NULL, NULL, &ftWrite)) {
                FileTimeToSystemTime(&ftWrite, &stUTC);
                SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
                wsprintfA(szMenuText, "Slot %d\t%04d/%02d/%02d %02d:%02d:%02d",
                    i, stLocal.wYear, stLocal.wMonth, stLocal.wDay,
                    stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
            } else wsprintfA(szMenuText, "Slot %d\tEmpty", i);
            CloseHandle(hFile);
        } else wsprintfA(szMenuText, "Slot %d\tEmpty", i);
        ModifyMenuA(hStateMenu, 40008 + i, MF_BYCOMMAND | MF_STRING, 40008 + i, szMenuText);
    }
    for (int i = 0; i < 10; ++i)
        CheckMenuItem(hStateMenu, 40008 + i, (i == g_active_slot) ? MF_CHECKED : MF_UNCHECKED);
}

// ============================================================================
// State save/load (v4)
// ============================================================================
struct StateWriter {
    FOut& f;
    template <class T> void operator()(const T& v) { f.write(&v, sizeof(T)); }
};
struct StateReader {
    FIn& f;
    template <class T> void operator()(T& v) { f.read(&v, sizeof(T)); }
};

bool saveStateToFile(const std::string& path) {
    FOut f(path.c_str());
    if (!f.ok()) return false;
    uint32_t magic = Bus::kStateMagic, version = Bus::kStateVersion;
    uint32_t romSize = (uint32_t)g_bus.rom.size();
    f.write(&magic, 4); f.write(&version, 4); f.write(&romSize, 4);
    StateWriter w{f};
    g_bus.serialize(w);
    return f.good();
}

bool loadStateFromFile(const std::string& path) {
    FIn f(path.c_str());
    if (!f.ok()) return false;
    uint32_t magic = 0, version = 0, romSize = 0;
    f.read(&magic, 4); f.read(&version, 4); f.read(&romSize, 4);
    if (!f.good() || magic != Bus::kStateMagic || version != Bus::kStateVersion || romSize != g_bus.rom.size())
        return false;
    static Bus tmp;
    tmp = g_bus;
    StateReader r{f};
    tmp.serialize(r);
    if (!f.good()) return false;
    tmp.apu.sampleCount = 0;
    g_bus = tmp;
    tmp.rom.clear();
    memcpy(g_screenBuffer, g_bus.frameBuf, sizeof(g_screenBuffer));
    return true;
}

void EjecutarSaveState(HWND hWnd) {
    if (!g_rom_loaded) return;
    CreateDirectoryA(".\\gbsave", NULL);
    char szPath[MAX_PATH];
    wsprintfA(szPath, ".\\gbsave\\%s.st%d", g_rom_base_name, g_active_slot);
    if (!saveStateToFile(std::string(szPath))) {
        MessageBoxA(hWnd, "Error al guardar el state.", "Rew. Reborn", MB_OK | MB_ICONERROR);
        return;
    }
    ActualizarMenuDeEstados(hWnd);
    MessageBoxA(hWnd, "State Guardado Correctamente.", "Rew. Reborn", MB_OK | MB_ICONINFORMATION);
}

void EjecutarLoadState(HWND hWnd) {
    if (!g_rom_loaded) return;
    char szPath[MAX_PATH];
    wsprintfA(szPath, ".\\gbsave\\%s.st%d", g_rom_base_name, g_active_slot);
    if (!loadStateFromFile(std::string(szPath))) {
        MessageBoxA(hWnd, "No se pudo cargar el state.\nPuede ser de otra version o de otra ROM.", "Rew. Reborn", MB_OK | MB_ICONERROR);
        return;
    }
    InvalidateRect(hWnd, NULL, FALSE);
    UpdateWindow(hWnd);
    MessageBoxA(hWnd, "State Cargado Correctamente.", "Rew. Reborn", MB_OK | MB_ICONINFORMATION);
}

bool TextoMenuCoincide(const char* menuText, const char* wantedText) {
    char cleaned[128] = {0};
    size_t j = 0;
    for (size_t i = 0; menuText[i] && j + 1 < sizeof(cleaned); ++i)
        if (menuText[i] != '&') cleaned[j++] = menuText[i];
    cleaned[j] = '\0';
    return lstrcmpiA(cleaned, wantedText) == 0;
}

bool BuscarComandoMenuPorTexto(HMENU hMenu, UINT commandId, const char* wantedText) {
    if (!hMenu) return false;
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        char itemText[128] = {0};
        GetMenuStringA(hMenu, (UINT)i, itemText, sizeof(itemText), MF_BYPOSITION);
        UINT itemId = GetMenuItemID(hMenu, i);
        if (itemId != (UINT)-1 && itemId == commandId && TextoMenuCoincide(itemText, wantedText)) return true;
        HMENU subMenu = GetSubMenu(hMenu, i);
        if (subMenu && BuscarComandoMenuPorTexto(subMenu, commandId, wantedText)) return true;
    }
    return false;
}
bool EsComandoMenu(HWND hWnd, UINT commandId, const char* wantedText) {
    return BuscarComandoMenuPorTexto(GetMenu(hWnd), commandId, wantedText);
}

void MostrarAcercaDe(HWND hWnd) {
    MessageBoxA(hWnd,
        "Rew. Reborn v0.8.3-alpha\n"
        "Emulador Game Boy / Game Boy Color\n"
        "Port Windows 9x/ME y Windows de 32 bits (i686)\n\n"
        "- Modos Super Game Boy con borde (256x224):\n"
        "  Game Boy, Game Boy Color + SGB Border y\n"
        "  Game Boy Color + Initial SGB Border\n"
        "- Bordes SGB reales: se decodifican los paquetes\n"
        "  CHR_TRN/PCT_TRN de la ROM (Pokemon Red/Gold,\n"
        "  Yellow, Telefang, etc.)\n"
        "- Los modos Game Boy Color + SGB Border y\n"
        "  Game Boy Color + Initial SGB Border muestran el\n"
        "  borde real del juego conservando el color CGB\n"
        "- El titulo de la ventana muestra el nombre del\n"
        "  cartucho cargado\n"
        "- Force SGB Border: muestra un borde aunque el\n"
        "  juego no incluya uno (estilos tipo Pokemon)\n"
        "- Bordes externos: <rom>.sgb / <rom>.bmp 256x224\n"
        "- Speedup Toggle: tecla mapeable que acelera\n"
        "  la emulaci\xF3n mientras se mantiene pulsada\n"
        "- Auto A / Auto B mapeables (disparo autom\xE1tico)\n"
        "- CPU con temporizaci\xF3n por M-ciclo\n"
        "- Timer, interrupciones, HALT y halt bug\n"
        "- PPU por dots, STAT por flanco, OAM DMA\n"
        "- Bug de corrupci\xF3n de OAM (modo DMG)\n"
        "- CGB: WRAM/VRAM banking, HDMA, paletas,\n"
        "  doble velocidad\n"
        "- MBC1 / MBC2 / MBC3 + RTC / MBC5\n"
        "- APU sincronizada con DIV\n"
        "- Save SRAM (+RTC) y Save States\n\n"
        "Tests de Blargg: cpu_instrs, instr_timing,\n"
        "mem_timing, mem_timing-2, halt_bug,\n"
        "interrupt_time, dmg_sound, cgb_sound y\n"
        "oam_bug (este \xFAltimo en modo DMG).",
        "About", MB_OK | MB_ICONINFORMATION);
}

void ActualizarAutoFire() {
    // [v0.4] AutoFire: toggle cada AUTO_FIRE_FRAMES frames
    if (g_auto_fire_enabled[0]) {
        g_auto_fire_counter[0]++;
        if (g_auto_fire_counter[0] >= AUTO_FIRE_FRAMES) {
            g_auto_fire_counter[0] = 0;
            // Toggle A
            bool pressed = ((g_bus.buttonsState >> 4) & 1) == 0;
            g_bus.setButton(4, !pressed);
        }
    }
    if (g_auto_fire_enabled[1]) {
        g_auto_fire_counter[1]++;
        if (g_auto_fire_counter[1] >= AUTO_FIRE_FRAMES) {
            g_auto_fire_counter[1] = 0;
            bool pressed = ((g_bus.buttonsState >> 5) & 1) == 0;
            g_bus.setButton(5, !pressed);
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    HMENU hM = GetMenu(hWnd);
    switch (msg) {
        case WM_INITMENUPOPUP:
            if (g_rom_loaded && reinterpret_cast<HMENU>(wp) == GetSubMenu(GetMenu(hWnd), 1))
                ActualizarMenuDeEstados(hWnd);
            break;

        case WM_KEYDOWN: {
            if ((static_cast<uint32_t>(lp) & (1u << 30)) != 0) break;
            uint32_t vk = (uint32_t)wp;
            for (int a = 0; a < MAX_ACTIONS; ++a)
                for (int c = 0; c < 4; ++c)
                    if (g_bindings[a][c] == vk && vk != 0) {
                        if (a <= 7) g_bus.setButton(a, true);
                        else if (a == 8) g_auto_fire_enabled[0] = true;   // Auto A
                        else if (a == 9) g_auto_fire_enabled[1] = true;   // Auto B
                        else if (a == 10) g_speedup_held = true;          // Speedup
                    }
            break;
        }
        case WM_KEYUP: {
            uint32_t vk = (uint32_t)wp;
            for (int a = 0; a < MAX_ACTIONS; ++a)
                for (int c = 0; c < 4; ++c)
                    if (g_bindings[a][c] == vk && vk != 0) {
                        if (a <= 7) g_bus.setButton(a, false);
                        else if (a == 8) { g_auto_fire_enabled[0] = false; g_bus.setButton(4, false); }
                        else if (a == 9) { g_auto_fire_enabled[1] = false; g_bus.setButton(5, false); }
                        else if (a == 10) g_speedup_held = false;
                    }
            break;
        }
        case WM_KILLFOCUS:
            g_bus.releaseAllButtons();
            g_auto_fire_enabled[0] = g_auto_fire_enabled[1] = false;
            g_speedup_held = false;
            break;

        case WM_COMMAND:
            if (EsComandoMenu(hWnd, LOWORD(wp), "Exit")) { DestroyWindow(hWnd); return 0; }
            if (EsComandoMenu(hWnd, LOWORD(wp), "About") || EsComandoMenu(hWnd, LOWORD(wp), "About...")) { MostrarAcercaDe(hWnd); return 0; }
            if (LOWORD(wp) == ID_HELP_ABOUT || LOWORD(wp) == ID_OPTION_AUDIO_ENABLE) { MostrarAcercaDe(hWnd); return 0; }
            if (LOWORD(wp) == ID_FILE_LOADROM) OpenRomDialog(hWnd);
            if (LOWORD(wp) == ID_FILE_FREEROM) LiberarRomActual(hWnd);
            if (LOWORD(wp) == ID_FILE_RESET)   ResetearEmulador(hWnd);
            if (LOWORD(wp) == ID_FILE_PAUSE)   AlternarPausaEmulador(hWnd);
            if (LOWORD(wp) == ID_OPTION_KEY) DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_KEY_SETTING), hWnd, KeySettingProc, 0);
            if (LOWORD(wp) == ID_AUDIO_TOGGLE) {
                g_audio_enabled = !g_audio_enabled;
                CheckMenuItem(hM, ID_AUDIO_TOGGLE, g_audio_enabled ? MF_CHECKED : MF_UNCHECKED);
                if (!g_audio_enabled && g_hWaveOut) waveOutReset(g_hWaveOut);
                GuardarConfiguracionIni();
            }
            if (LOWORD(wp) >= ID_SYSTEM_AUTO &&
                LOWORD(wp) < ID_SYSTEM_AUTO + SYSTEM_MODE_COUNT) {
                int pref = LOWORD(wp) - ID_SYSTEM_AUTO;
                CheckMenuRadioItem(hM, ID_SYSTEM_AUTO, ID_SYSTEM_AUTO + SYSTEM_MODE_COUNT - 1,
                                   LOWORD(wp), MF_BYCOMMAND);
                if (pref != g_model_pref) {
                    g_model_pref = pref;
                    if (!g_rom_loaded) {
                        PrepararBordeParaModo(pref);
                        ResizeEmulatorWindow(hWnd, g_window_scale);
                    }
                    GuardarConfiguracionIni();
                    if (g_rom_loaded) {
                        char path[MAX_PATH];
                        lstrcpyA(path, g_rom_full_path);
                        CargarRomDesdeRuta(hWnd, path);
                    }
                }
            }
            if (LOWORD(wp) == ID_SGB_FORCE) {
                g_force_sgb_border = !g_force_sgb_border;
                CheckMenuItem(hM, ID_SGB_FORCE, g_force_sgb_border ? MF_CHECKED : MF_UNCHECKED);
                InitSgbBorder(g_model_pref);
                ResizeEmulatorWindow(hWnd, g_window_scale);
                GuardarConfiguracionIni();
            }
            if (LOWORD(wp) == ID_SGB_THEME_AUTO) {
                g_sgb_theme_pref = -1;
                CheckMenuRadioItem(hM, ID_SGB_THEME_AUTO, ID_SGB_THEME_BASE + SGB_THEME_COUNT - 1,
                                   ID_SGB_THEME_AUTO, MF_BYCOMMAND);
                InitSgbBorder(g_model_pref);
                GuardarConfiguracionIni();
            }
            if (LOWORD(wp) >= ID_SGB_THEME_BASE && LOWORD(wp) < ID_SGB_THEME_BASE + SGB_THEME_COUNT) {
                g_sgb_theme_pref = LOWORD(wp) - ID_SGB_THEME_BASE;
                CheckMenuRadioItem(hM, ID_SGB_THEME_AUTO, ID_SGB_THEME_BASE + SGB_THEME_COUNT - 1,
                                   LOWORD(wp), MF_BYCOMMAND);
                if (!g_force_sgb_border) { g_force_sgb_border = true; CheckMenuItem(hM, ID_SGB_FORCE, MF_CHECKED); }
                InitSgbBorder(g_model_pref);
                GuardarConfiguracionIni();
            }
            if (LOWORD(wp) == ID_STATE_SAVE) { if (g_rom_loaded) EjecutarSaveState(hWnd); else MessageBoxA(hWnd, "Carga una ROM primero.", "Aviso", MB_OK); }
            if (LOWORD(wp) == ID_STATE_LOAD) { if (g_rom_loaded) EjecutarLoadState(hWnd); else MessageBoxA(hWnd, "Carga una ROM primero.", "Aviso", MB_OK); }
            if (LOWORD(wp) >= ID_SLOT_0 && LOWORD(wp) <= ID_SLOT_9) {
                g_active_slot = LOWORD(wp) - ID_SLOT_0;
                for (int i = ID_SLOT_0; i <= ID_SLOT_9; ++i) CheckMenuItem(hM, i, MF_UNCHECKED);
                CheckMenuItem(hM, LOWORD(wp), MF_CHECKED);
            }
            if (LOWORD(wp) >= ID_VIDEO_X1 && LOWORD(wp) <= ID_VIDEO_X4) {
                int scale = LOWORD(wp) - ID_VIDEO_X1 + 1;
                CheckMenuRadioItem(hM, ID_VIDEO_X1, ID_VIDEO_X4, LOWORD(wp), MF_BYCOMMAND);
                ResizeEmulatorWindow(hWnd, scale);
                g_window_scale = scale;
                GuardarConfiguracionIni();
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT cr;
            GetClientRect(hWnd, &cr);
            SetStretchBltMode(hdc, COLORONCOLOR);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = g_sgb_border_active ? SGB_W : GB_WIDTH;
            bmi.bmiHeader.biHeight = g_sgb_border_active ? -SGB_H : -GB_HEIGHT;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(hdc, 0, 0, cr.right, cr.bottom,
                          0, 0, bmi.bmiHeader.biWidth, -bmi.bmiHeader.biHeight,
                          g_sgb_border_active ? g_sgbBuffer : g_screenBuffer,
                          &bmi, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(hWnd, &ps);
            break;
        }

        case WM_DESTROY:
            GuardarSramCartucho();
            GuardarConfiguracionIni();
            CerrarAudio();
			timeEndPeriod(1);
            PostQuitMessage(0);
            break;
        default: return DefWindowProcA(hWnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
	timeBeginPeriod(1); 
	CargarConfiguracionIni();
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "RewRebornClass";
    wc.lpszMenuName = MAKEINTRESOURCEA(IDR_MAIN_MENU);
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP_ICON));
	INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    RegisterClassExA(&wc);

    RECT rc = {0, 0, (g_sgb_border_active ? SGB_W : GB_WIDTH) * g_window_scale,
               (g_sgb_border_active ? SGB_H : GB_HEIGHT) * g_window_scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);
    HWND hWnd = CreateWindowExA(0, "RewRebornClass", "Rew. Reborn v0.8.3-alpha", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);
    ShowWindow(hWnd, nShow); UpdateWindow(hWnd);

    // [v0.4] Init audio
    IniciarAudio(hWnd);
    CheckMenuRadioItem(GetMenu(hWnd), ID_VIDEO_X1, ID_VIDEO_X4,
                       ID_VIDEO_X1 + (g_window_scale - 1), MF_BYCOMMAND);
    CrearMenusExtra(hWnd);

    LARGE_INTEGER qpcFreq, lastFrameTime;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&lastFrameTime);
    const LONGLONG ticksPerFrame = (LONGLONG)qpcFreq.QuadPart * 70224 / 4194304;

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));	
    LARGE_INTEGER fpsTimerStart;
    QueryPerformanceCounter(&fpsTimerStart);
    int fpsFrameCount = 0;	
    while (msg.message != WM_QUIT) {
        if (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } else {
            if (g_rom_loaded && !g_emulation_paused) {
                // Speedup: mientras la tecla esta pulsada se ejecutan M frames
                // por fotograma, pero el ritmo se mantiene estable (objetivo de
                // tiempo real a M velocidad) para que el audio no pierda el
                // sincronismo. El audio de waveOut se acelera a la misma tasa;
                // si el dispositivo no lo admite, se silencia durante el
                // speedup. Al soltar la tecla se corta el lote en curso.
                if (g_speedup_held && !g_speedup_prev) {
                    g_speedup_prev = true;
                    g_speedup_rate_active = false;
                    g_speedup_audio_muted = false;
                    if (!AplicarTasaSpeedup(true)) g_speedup_audio_muted = true;
                } else if (!g_speedup_held && g_speedup_prev) {
                    g_speedup_prev = false;
                    if (!g_speedup_audio_muted) AplicarTasaSpeedup(false);
                    g_speedup_audio_muted = false;
                    g_speedup_rate_active = false;
                    g_bus.apu.sampleCount = 0;
                    QueryPerformanceCounter(&lastFrameTime);
                }
                LONGLONG ticksPerEmuFrame = g_speedup_held
                    ? (LONGLONG)(qpcFreq.QuadPart * 70224 / (4194304.0 * SGB_SPEEDUP_MULT))
                    : ticksPerFrame;
                int framesThisTick = g_speedup_held ? SGB_SPEEDUP_MULT : 1;
                int framesRun = 0;
                for (int f = 0; f < framesThisTick; ++f) {
                    if (f > 0 && !g_speedup_held) break;
                    g_bus.runFrame();
                    framesRun++;
                    if (!(g_bus.lcdc & 0x80)) memcpy(g_screenBuffer, g_bus.frameBuf, sizeof(g_screenBuffer));
                    if (g_sgb_border_active) ComponerFrameSgb();
                    // [v0.4] Auto Fire
                    ActualizarAutoFire();
                }
                // Si el juego envio su borde SGB (CHR_TRN/PCT_TRN) tras arrancar,
                // se aplica una sola vez y se reencuadra la ventana. Tiene
                // prioridad sobre el estilo de reserva (index >= 0).
                if (ModeUsesBorder(g_model_pref) && !g_sgb_border_checked &&
                    g_bus.sgbBorderFromPacket && (!g_sgb_border_active || g_sgb_border_index >= 0)) {
                    g_sgb_border_checked = true;
                    InitSgbBorder(g_model_pref);
                    ResizeEmulatorWindow(hWnd, g_window_scale);
                }
                // Modos CGB+SGB: si al cargar la ROM todavia no habia cartucho (o
                // la captura fallo), se intenta capturar el borde real ahora.
                if (g_sgb_capture_pending && g_rom_loaded && !g_bus.rom.empty()) {
                    g_sgb_capture_pending = false;
                    if (CapturarBordeSgbParaCgb()) {
                        InitSgbBorder(g_model_pref);
                        ResizeEmulatorWindow(hWnd, g_window_scale);
                    }
                }
                EnviarFrameAudio();				
                InvalidateRect(hWnd, NULL, FALSE);
                UpdateWindow(hWnd);

                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                // Ritmo estable: se espera a que transcurra el tiempo de los
                // frames ejecutados a la velocidad actual (normal o M).
                LONGLONG objetivo = lastFrameTime.QuadPart + ticksPerEmuFrame * framesRun;
                if (now.QuadPart < objetivo) {
                    LONGLONG restante = objetivo - now.QuadPart;
                    DWORD sleepMs = (DWORD)(restante / (qpcFreq.QuadPart / 1000));
                    if (sleepMs > 2) Sleep(sleepMs - 1);
                    do { QueryPerformanceCounter(&now); } while (now.QuadPart < objetivo);
                }
                // ★ Contador de FPS (actualiza cada 1 segundo)
                fpsFrameCount += framesRun;
                LARGE_INTEGER fpsNow;
                QueryPerformanceCounter(&fpsNow);
                double elapsedSec = (double)(fpsNow.QuadPart - fpsTimerStart.QuadPart) / qpcFreq.QuadPart;
                if (elapsedSec >= 1.0) {
                    double fps = fpsFrameCount / elapsedSec;
                    int fps10 = (int)(fps * 10);  // un decimal
                    char titulo[MAX_PATH * 2];
                    if (g_rom_display_name[0])
                        wsprintfA(titulo, "%s - %s - %d.%d FPS%s", g_window_base_title,
                                  g_rom_display_name, fps10 / 10, fps10 % 10,
                                  g_speedup_held ? " [Speedup]" : "");
                    else
                        wsprintfA(titulo, "%s - %d.%d FPS%s", g_window_base_title,
                                  fps10 / 10, fps10 % 10, g_speedup_held ? " [Speedup]" : "");
                    SetWindowTextA(hWnd, titulo);
                    fpsFrameCount = 0;
                    QueryPerformanceCounter(&fpsTimerStart);
                }				
                lastFrameTime = now;
            } else {
                Sleep(10);
            }
        }
    }
    return (int)msg.wParam;
}

#endif // REW_HEADLESS
