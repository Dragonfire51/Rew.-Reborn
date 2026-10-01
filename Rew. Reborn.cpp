#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include <cstdint>
#include <vector>
#include <array>
#include <fstream>
#include <string>
#include <cstring>
#include <algorithm>
#include "resource.h"
const int GB_WIDTH = 160;
const int GB_HEIGHT = 144;
uint32_t g_screenBuffer[GB_WIDTH * GB_HEIGHT] = {0};
bool g_rom_loaded = false;
int g_active_slot = 0;
bool g_emulation_paused = false;
char g_rom_base_name[MAX_PATH] = "State";
int  g_window_scale = 2;

// [v0.4] Audio enable + master volume
bool g_audio_enabled = true;

// [v0.4] Auto-fire
bool g_auto_fire_enabled[2] = { false, false };   // 0=A, 1=B
int  g_auto_fire_counter[2]  = { 0, 0 };
const int AUTO_FIRE_FRAMES = 10;  // toggle cada 10 frames (~6 Hz)

// [v0.4] 40 bindings: 10 actions x 4 slots. Actions:
// 0=UP 1=DOWN 2=LEFT 3=RIGHT 4=A 5=B 6=AutoA 7=AutoB 8=Start 9=Select
DWORD g_bindings[8][4] = {
    { VK_UP,    0, 0, 0 }, { VK_DOWN,  0, 0, 0 },
    { VK_LEFT,  0, 0, 0 }, { VK_RIGHT, 0, 0, 0 },
    { 'Z',      0, 0, 0 }, { 'X',      0, 0, 0 },
    { VK_SPACE, 0, 0, 0 }, { VK_RETURN,0, 0, 0 }
};
const char* g_action_names[8] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "Select", "Start"
};

WNDPROC g_origEditProc[10][4] = { nullptr };

void ActualizarMenuDeEstados(HWND hWnd);

// ============================================================================
// [v0.4] APU
// ============================================================================
class APU {
public:
    uint8_t nr10=0x80,nr11=0xBF,nr12=0xF3,nr13=0xFF,nr14=0xBF;
    uint8_t nr21=0x3F,nr22=0x00,nr23=0xFF,nr24=0xBF;
    uint8_t nr30=0x7F,nr31=0xFF,nr32=0x9F,nr33=0xFF,nr34=0xBF;
    uint8_t nr41=0xFF,nr42=0x00,nr43=0x00,nr44=0xBF;
    uint8_t nr50=0x77,nr51=0xF3,nr52=0xF1;
    uint8_t waveRam[16] = {0};

    struct Ch {
        bool enabled=false, dacEnabled=false;
        bool lengthEnabled=false;
        int timer=0, freq=0, dutyPos=0, duty=0, vol=0;
        int lengthCounter=0;
        int sweepTimer=0;
        int envTimer=0;
        int wavePos=0;
        int lfsr=0x7FFF, divisor=0, shift=0;
        bool widthMode=false;
		int dacOutput = 0;
    } c1, c2, c3, c4;

    int frameSeqCounter = 0;
    int frameSeqStep = 0;

    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int SAMPLES_PER_FRAME = 800;
    double cycleAcc = 0;
    int16_t sampleBuf[SAMPLES_PER_FRAME * 2];   // interleaved L,R,L,R,...
    int sampleCount = 0;

    int32_t lpFilterL = 0, lpFilterR = 0;
    int32_t dcPrevInL = 0, dcPrevOutL = 0;
    int32_t dcPrevInR = 0, dcPrevOutR = 0;
	int32_t hpFilterState = 0;
	int32_t hpLastIn = 0;
    int32_t hpLastOut = 0;

    static constexpr uint8_t DUTY[4][8] = {
        {0,0,0,0,0,0,0,1},
        {1,0,0,0,0,0,0,1},
        {1,0,0,0,0,1,1,1},
        {0,1,1,1,1,1,1,0}
    };

    void reset() {
        nr10=0x80;nr11=0xBF;nr12=0xF3;nr13=0xFF;nr14=0xBF;
        nr21=0x3F;nr22=0x00;nr23=0xFF;nr24=0xBF;
        nr30=0x7F;nr31=0xFF;nr32=0x9F;nr33=0xFF;nr34=0xBF;
        nr41=0xFF;nr42=0x00;nr43=0x00;nr44=0xBF;
        nr50=0x77;nr51=0xF3;nr52=0xF1;
        for (int i=0;i<16;++i) waveRam[i]=0;
        c1=c2=c3=c4=Ch(); c4.lfsr=0x7FFF;
        cycleAcc=0; sampleCount=0;
        frameSeqCounter=0; frameSeqStep=0;
        lpFilterL = 0; lpFilterR = 0;
        dcPrevInL = 0; dcPrevOutL = 0;
        dcPrevInR = 0; dcPrevOutR = 0; 
		hpFilterState = 0;
		hpLastIn = 0;
        hpLastOut = 0;
		c1.dacOutput = 0;
        c2.dacOutput = 0;
        c3.dacOutput = 0;
        c4.dacOutput = 0;
    }

    // ──────── Frame sequencer helpers ────────
    void clockLength(Ch& ch, int maxLen) {
        (void)maxLen;
        if (!ch.lengthEnabled) return;
        if (ch.lengthCounter > 0) {
            ch.lengthCounter--;
            if (ch.lengthCounter == 0) ch.enabled = false;
        }
    }

    void clockEnvelope(Ch& ch) {
        if (!ch.enabled) return;
        int period, dir;
        if (&ch == &c1)      { period = nr12 & 7; dir = (nr12 >> 3) & 1; }
        else if (&ch == &c2) { period = nr22 & 7; dir = (nr22 >> 3) & 1; }
        else                 { period = nr42 & 7; dir = (nr42 >> 3) & 1; }
        if (period == 0) return;
        if (--ch.envTimer <= 0) {
            ch.envTimer = period;
            if (dir) { if (ch.vol < 15) ch.vol++; }
            else     { if (ch.vol > 0)  ch.vol--; }
        }
    }

    void clockSweep() {
        if (!c1.enabled) return;
        int period = (nr10 >> 4) & 7;
        if (period == 0) return;
        if (--c1.sweepTimer <= 0) {
            c1.sweepTimer = period;
            int shift = nr10 & 7;
            if (shift == 0) return;
            int delta = c1.freq >> shift;
            int newFreq = (nr10 & 0x08) ? (c1.freq - delta) : (c1.freq + delta);
            if (newFreq > 2047) c1.enabled = false;
            else if (newFreq >= 0) c1.freq = newFreq;
        }
    }

    void tickFrameSeq() {
        frameSeqStep = (frameSeqStep + 1) & 7;
        if ((frameSeqStep & 1) == 0) {
            clockLength(c1, 64);
            clockLength(c2, 64);
            clockLength(c3, 256);
            clockLength(c4, 64);
        }
        if (frameSeqStep == 2 || frameSeqStep == 6) clockSweep();
        if (frameSeqStep == 7) {
            clockEnvelope(c1);
            clockEnvelope(c2);
            clockEnvelope(c4);
        }
    }

    // ──────── Registros ────────
    uint8_t readReg(uint16_t addr) {
        // ★ Si APU está apagada, la mayoría de registros leen 0x00
        if (!(nr52 & 0x80)) {
            if (addr == 0xFF26) return nr52 | 0x70;   // NR52 siempre se lee
            if (addr >= 0xFF30 && addr <= 0xFF3F) return waveRam[addr - 0xFF30];
            // Los demás leen 0x00 (incluso NR11/NR21 pierden los bits de duty)
            return 0x00;
        }

        // APU encendida: máscaras normales
        switch(addr){
            case 0xFF10: return nr10|0x80;
            case 0xFF11: return nr11|0x3F;
            case 0xFF12: return nr12;
            case 0xFF13: return 0xFF;
            case 0xFF14: return nr14|0xBF;
            case 0xFF15: return 0xFF;
            case 0xFF16: return nr21|0x3F;
            case 0xFF17: return nr22;
            case 0xFF18: return 0xFF;
            case 0xFF19: return nr24|0xBF;
            case 0xFF1A: return nr30|0x7F;
            case 0xFF1B: return 0xFF;
            case 0xFF1C: return nr32|0x9F;
            case 0xFF1D: return 0xFF;
            case 0xFF1E: return nr34|0xBF;
            case 0xFF1F: return 0xFF;
            case 0xFF20: return 0xFF;
            case 0xFF21: return nr42;
            case 0xFF22: return nr43;
            case 0xFF23: return nr44|0xBF;
            case 0xFF24: return nr50;
            case 0xFF25: return nr51;
            case 0xFF26: {
                uint8_t r = nr52 | 0x70;
                if (c1.enabled) r |= 0x01;
                if (c2.enabled) r |= 0x02;
                if (c3.enabled) r |= 0x04;
                if (c4.enabled) r |= 0x08;
                return r;
            }
            default:
                if (addr >= 0xFF30 && addr <= 0xFF3F) return waveRam[addr - 0xFF30];
                return 0xFF;
        }
    }

    void writeReg(uint16_t addr, uint8_t val) {
        if (addr >= 0xFF30 && addr <= 0xFF3F) { waveRam[addr - 0xFF30] = val; return; }
        switch(addr) {
            case 0xFF10: nr10 = val & 0x7F; return;

            case 0xFF11: nr11 = val; c1.duty = (val >> 6) & 3;
                         c1.lengthCounter = 64 - (val & 0x3F); return;

            case 0xFF12: {
                nr12 = val; c1.dacEnabled = (val & 0xF8) != 0;
                if (!c1.dacEnabled) c1.enabled = false;
                c1.vol = (val >> 4) & 0x0F;
                return;
            }

            case 0xFF13: nr13 = val; c1.freq = (c1.freq & 0x700) | val; return;

            case 0xFF14: {
                nr14 = val & 0x7F;
                c1.freq = (c1.freq & 0xFF) | ((val & 7) << 8);
                bool wasLenEnabled = c1.lengthEnabled;
                c1.lengthEnabled = (val & 0x40) != 0;

                // ★ Trigger PRIMERO
                if (val & 0x80) {
                    c1.enabled = c1.dacEnabled;
                    if (c1.lengthCounter == 0) c1.lengthCounter = 64;
                    c1.timer = (2048 - c1.freq) * 4;
                    if (c1.timer <= 0) c1.timer = 4;
                    c1.dutyPos = 0;
                    c1.envTimer = (nr12 & 7) ? (nr12 & 7) : 8;
                    c1.sweepTimer = ((nr10 >> 4) & 7) ? ((nr10 >> 4) & 7) : 8;
                }
                // ★ Extra clock DESPUÉS, si estamos en la primera mitad del período
                if (!wasLenEnabled && c1.lengthEnabled && (frameSeqStep % 2) == 0) {
                    clockLength(c1, 64);
                }
                return;
            }

            case 0xFF16: nr21 = val; c2.duty = (val >> 6) & 3;
                         c2.lengthCounter = 64 - (val & 0x3F); return;

            case 0xFF17: {
                nr22 = val; c2.dacEnabled = (val & 0xF8) != 0;
                if (!c2.dacEnabled) c2.enabled = false;
                c2.vol = (val >> 4) & 0x0F;
                return;
            }

            case 0xFF18: nr23 = val; c2.freq = (c2.freq & 0x700) | val; return;

            case 0xFF19: {
                nr24 = val & 0x7F;
                c2.freq = (c2.freq & 0xFF) | ((val & 7) << 8);
                bool wasLenEnabled = c2.lengthEnabled;
                c2.lengthEnabled = (val & 0x40) != 0;

                if (val & 0x80) {
                    c2.enabled = c2.dacEnabled;
                    if (c2.lengthCounter == 0) c2.lengthCounter = 64;
                    c2.timer = (2048 - c2.freq) * 4;
                    if (c2.timer <= 0) c2.timer = 4;
                    c2.dutyPos = 0;
                    c2.envTimer = (nr22 & 7) ? (nr22 & 7) : 8;
                }
                if (!wasLenEnabled && c2.lengthEnabled && (frameSeqStep % 2) == 0) {
                    clockLength(c2, 64);
                }
                return;
            }

            case 0xFF1A: nr30 = val & 0x7F; c3.dacEnabled = (val & 0x80) != 0;
                         if (!c3.dacEnabled) c3.enabled = false; return;

            case 0xFF1B: nr31 = val; c3.lengthCounter = 256 - val; return;
            case 0xFF1C: nr32 = val; c3.vol = (val >> 5) & 3; return;
            case 0xFF1D: nr33 = val; c3.freq = (c3.freq & 0x700) | val; return;

            case 0xFF1E: {
                nr34 = val & 0x7F;
                c3.freq = (c3.freq & 0xFF) | ((val & 7) << 8);
                bool wasLenEnabled = c3.lengthEnabled;
                c3.lengthEnabled = (val & 0x40) != 0;

                if (val & 0x80) {
                    c3.enabled = c3.dacEnabled;
                    if (c3.lengthCounter == 0) c3.lengthCounter = 256;
                    c3.timer = (2048 - c3.freq) * 2;
                    if (c3.timer <= 0) c3.timer = 2;
                    c3.wavePos = 0;
                }
                if (!wasLenEnabled && c3.lengthEnabled && (frameSeqStep % 2) == 0) {
                    clockLength(c3, 256);
                }
                return;
            }

            case 0xFF20: nr41 = val; c4.lengthCounter = 64 - (val & 0x3F); return;

            case 0xFF21: {
                nr42 = val; c4.dacEnabled = (val & 0xF8) != 0;
                if (!c4.dacEnabled) c4.enabled = false;
                c4.vol = (val >> 4) & 0x0F;
                return;
            }

            case 0xFF22: nr43 = val; c4.shift = (val >> 4) & 0x0F;
                         c4.widthMode = (val & 8) != 0; c4.divisor = val & 7; return;

            case 0xFF23: {
                nr44 = val & 0x7F;
                bool wasLenEnabled = c4.lengthEnabled;
                c4.lengthEnabled = (val & 0x40) != 0;

                if (val & 0x80) {
                    c4.enabled = c4.dacEnabled;
                    if (c4.lengthCounter == 0) c4.lengthCounter = 64;
                    c4.lfsr = 0x7FFF;
                    c4.envTimer = (nr42 & 7) ? (nr42 & 7) : 8;
                }
                if (!wasLenEnabled && c4.lengthEnabled && (frameSeqStep % 2) == 0) {
                    clockLength(c4, 64);
                }
                return;
            }

            case 0xFF24: nr50 = val; return;
            case 0xFF25: nr51 = val; return;

            case 0xFF26: {
                nr52 = val & 0x80;
                if (!(val & 0x80)) {
                    c1.enabled=c2.enabled=c3.enabled=c4.enabled=false;
                    nr10=nr11=nr12=nr13=nr14=0;
                    nr21=nr22=nr23=nr24=0;
                    nr30=nr31=nr32=nr33=nr34=0;
                    nr41=nr42=nr43=nr44=0;
                }
                return;
            }
        }
    }

    void tick(int tCycles) {
        static constexpr double cyclesPerOversample = 4194304.0 / (SAMPLE_RATE * 4);
        int remaining = tCycles;
        int32_t sumL = 0, sumR = 0;
        int oversampleCount = 0;

        while (remaining > 0) {
            int step = (int)(cyclesPerOversample - cycleAcc) + 1;
            if (step < 1) step = 1;
            if (step > remaining) step = remaining;

            int fsRemaining = 8192 - frameSeqCounter;
            if (fsRemaining < step) step = fsRemaining;

            advancePulse(c1, step);
            advancePulse(c2, step);
            advanceWave(step);
            advanceNoise(step);

            cycleAcc += step;
            frameSeqCounter += step;
            remaining -= step;

            if (frameSeqCounter >= 8192) {
                frameSeqCounter -= 8192;
                tickFrameSeq();
            }

            if (cycleAcc >= cyclesPerOversample) {
                cycleAcc -= cyclesPerOversample;

                int32_t rawL, rawR;
                generateSampleLR(rawL, rawR);

                // Filtro IIR anti-aliasing por canal
                lpFilterL += (rawL - lpFilterL) / 2;
                lpFilterR += (rawR - lpFilterR) / 2;

                sumL += lpFilterL;
                sumR += lpFilterR;
                oversampleCount++;

                if (oversampleCount >= 4) {
                    int32_t avgL = sumL / 4;
                    int32_t avgR = sumR / 4;

                    // HPF DC-block por canal
                    int64_t hpL = (int64_t)avgL - dcPrevInL + ((int64_t)dcPrevOutL * 998943) / 1000000;
                    int64_t hpR = (int64_t)avgR - dcPrevInR + ((int64_t)dcPrevOutR * 998943) / 1000000;
                    dcPrevInL = avgL; dcPrevOutL = hpL;
                    dcPrevInR = avgR; dcPrevOutR = hpR;

                    if (hpL < -32768) hpL = -32768;
                    if (hpL > 32767) hpL = 32767;
                    if (hpR < -32768) hpR = -32768;
                    if (hpR > 32767) hpR = 32767;

                    if (sampleCount + 1 < SAMPLES_PER_FRAME * 2) {
                        sampleBuf[sampleCount++] = (int16_t)hpL;
                        sampleBuf[sampleCount++] = (int16_t)hpR;
                    }

                    sumL = 0; sumR = 0;
                    oversampleCount = 0;
                }
            }
        }
    }

    void advancePulse(Ch& ch, int cycles) {
        if (!ch.enabled) return;
        ch.timer -= cycles;
        while (ch.timer <= 0) {
            ch.timer += (2048 - ch.freq) * 4;
            ch.dutyPos = (ch.dutyPos + 1) & 7;
        }
    }

    void advanceWave(int cycles) {
        if (!c3.enabled) return;
        c3.timer -= cycles;
        while (c3.timer <= 0) {
            c3.timer += (2048 - c3.freq) * 2;
            c3.wavePos = (c3.wavePos + 1) & 31;
        }
    }

    void advanceNoise(int cycles) {
        if (!c4.enabled) return;
        int divisor = c4.divisor == 0 ? 8 : (c4.divisor * 16);
        int period = divisor << c4.shift;
        if (period < 4) period = 4;
        c4.timer -= cycles;
        while (c4.timer <= 0) {
            c4.timer += period;
            int bit = (c4.lfsr & 1) ^ ((c4.lfsr >> 1) & 1);
            c4.lfsr = (c4.lfsr >> 1) | (bit << 14);
            if (c4.widthMode) {
                c4.lfsr = (c4.lfsr & ~(1u << 6)) | (bit << 6);
            }
        }
    }
	
    void generateSampleLR(int32_t& outL, int32_t& outR) {
        int ch1Out = 0, ch2Out = 0, ch3Out = 0, ch4Out = 0;

        // Canal 1 - slew limiter
        {
            int target;
            if (c1.dacEnabled) {
                if (c1.enabled) {
                    int d = DUTY[c1.duty][c1.dutyPos];
                    target = d ? (15 - c1.vol * 2) : 15;
                } else target = 15;
            } else target = 0;
            int diff = target - c1.dacOutput;
            if (diff > 4) diff = 4;
            if (diff < -4) diff = -4;
            c1.dacOutput += diff;
            ch1Out = c1.dacOutput;
        }

        // Canal 2
        {
            int target;
            if (c2.dacEnabled) {
                if (c2.enabled) {
                    int d = DUTY[c2.duty][c2.dutyPos];
                    target = d ? (15 - c2.vol * 2) : 15;
                } else target = 15;
            } else target = 0;
            int diff = target - c2.dacOutput;
            if (diff > 4) diff = 4;
            if (diff < -4) diff = -4;
            c2.dacOutput += diff;
            ch2Out = c2.dacOutput;
        }

        // Canal 3
        {
            int target;
            if (c3.dacEnabled) {
                if (c3.enabled) {
                    uint8_t byte = waveRam[(c3.wavePos >> 1) & 15];
                    int sample = (c3.wavePos & 1) ? (byte & 0x0F) : (byte >> 4);
                    int shift = c3.vol == 0 ? 4 : (c3.vol - 1);
                    sample >>= shift;
                    target = 15 - sample * 2;
                } else target = 15;
            } else target = 0;
            int diff = target - c3.dacOutput;
            if (diff > 4) diff = 4;
            if (diff < -4) diff = -4;
            c3.dacOutput += diff;
            ch3Out = c3.dacOutput;
        }

        // Canal 4
        {
            int target;
            if (c4.dacEnabled) {
                if (c4.enabled) {
                    int bit = ~c4.lfsr & 1;
                    target = bit ? (15 - c4.vol * 2) : 15;
                } else target = 15;
            } else target = 0;
            int diff = target - c4.dacOutput;
            if (diff > 4) diff = 4;
            if (diff < -4) diff = -4;
            c4.dacOutput += diff;
            ch4Out = c4.dacOutput;
        }

        // Routing estéreo NR51
        int left = 0, right = 0;
        if (nr51 & 0x10) left += ch1Out;
        if (nr51 & 0x20) left += ch2Out;
        if (nr51 & 0x40) left += ch3Out;
        if (nr51 & 0x80) left += ch4Out;
        if (nr51 & 0x01) right += ch1Out;
        if (nr51 & 0x02) right += ch2Out;
        if (nr51 & 0x04) right += ch3Out;
        if (nr51 & 0x08) right += ch4Out;

        // Volumen master NR50 (L y R independientes)
        int lv = ((nr50 >> 4) & 7) + 1;
        int rv = (nr50 & 7) + 1;
        outL = (left * lv) * 32;
        outR = (right * rv) * 32;
    }
};

// ============================================================================
// Bus
// ============================================================================
class Bus {
public:
    std::vector<uint8_t> rom;
    std::array<uint8_t, 0x2000> vram{}, vram1{};
    std::array<uint8_t, 0x20000> ext_ram{};
    std::array<uint8_t, 0x2000> wram{};                         // SVBK ($FF70)                                    // SVBK ($FF70), 0 se trata como 1
    std::array<uint8_t, 0xA0>   oam{};
    std::array<uint8_t, 0x80>   io{};
    std::array<uint8_t, 0x80>   hram{};
    uint8_t ie = 0;

    uint16_t romBank = 1;
    bool    ramEnabled = false;
    uint8_t ramBank = 0;
    uint8_t mbcMode = 0;
    uint8_t mbcType = 0;
    // [v0.4] MBC3 RTC
    bool rtcSelected = false;
    uint8_t rtcReg = 0;
    bool rtcLatchRequested = false;
    uint8_t rtcRegs[5] = {0,0,0,0,0};      // S, M, H, DL, DH (valores "vivos")
    uint8_t rtcLatched[5] = {0,0,0,0,0};   // valores capturados por latch
    bool    rtcLatched_valid = false;
    bool    rtcHalt = false;
    uint32_t rtcLastUpdate = 0;            // timestamp de la última actualización
    bool dmaActive = false;
    int  dmaCyclesRemaining = 0;
	    // HDMA state
    uint16_t hdmaSrc = 0;
    uint16_t hdmaDst = 0;
    uint8_t  hdmaBlocksLeft = 0;
    bool     hdmaActive = false;
    uint8_t  hdmaBank = 0;   // ★ VRAM bank latched al iniciar HDMA/GDMA
    // KEY1 - CGB speed switch ($FF4D)
    uint8_t  key1Armed = 0;
    bool     cgbDoubleSpeed = false;
    uint8_t joypSelect = 0x30;
    uint8_t buttonsState = 0xFF;
    static constexpr uint8_t kButtonBit[8] = { 2, 3, 1, 0, 4, 5, 6, 7 };

    uint16_t pc = 0x100;
    uint16_t sp = 0xFFFE;
    uint8_t a = 0x01, f = 0xB0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    bool ime = false, imeEnablePending = false, halted = false, haltBugPending = false;

    uint16_t divCounter = 0;
    uint8_t tima = 0, tma = 0, tac = 0;
    bool timaOverflowPending = false;
    int timaOverflowDelay = 0;
    static constexpr std::array<int, 4> kClockSelectBit = {9, 3, 5, 7};

    static constexpr uint8_t FLAG_Z = 0x80, FLAG_N = 0x40, FLAG_H = 0x20, FLAG_C = 0x10;
    uint8_t ly = 0;

    // [v0.4] APU
    APU apu;

    static constexpr uint32_t kStateMagic = 0x42574552;
    static constexpr uint32_t kStateVersion = 4;
	
    void UpdateRtc() {
        // Actualiza el RTC "vivo" basado en el tiempo real transcurrido
        DWORD now = GetTickCount();
        if (rtcLastUpdate == 0) { rtcLastUpdate = now; return; }
        DWORD elapsed = now - rtcLastUpdate;
        rtcLastUpdate = now;

        if (rtcHalt) return;   // RTC detenido por bit 6 del DH

        // Convertir milisegundos a segundos
        uint32_t seconds = elapsed / 1000;

        uint32_t s = rtcRegs[0];
        uint32_t m = rtcRegs[1];
        uint32_t h = rtcRegs[2];
        uint32_t dl = rtcRegs[3];
        uint32_t dh = rtcRegs[4] & 0x01;   // bit 0 del DH es el bit 8 de días

        uint32_t total = s + m * 60 + h * 3600 + (dl + dh * 256) * 86400;
        total += seconds;

        // Descomponer
        rtcRegs[0] = total % 60;
        total /= 60;
        rtcRegs[1] = total % 60;
        total /= 60;
        rtcRegs[2] = total % 24;
        total /= 24;

        // Días (pueden llegar a 511)
        if (total > 511) total &= 0x1FF;
        rtcRegs[3] = total & 0xFF;
        uint8_t newDh = (rtcRegs[4] & 0xFE) | ((total >> 8) & 0x01);

        // Si los días llegaron a 512, activar bit 7 (day carry)
        if (total >= 512) newDh |= 0x80;
        rtcRegs[4] = newDh;
    }
	
    bool loadRom(const std::string& path) {
        std::ifstream f_rom(path, std::ios::binary | std::ios::ate);
        if (!f_rom) return false;
        auto size = f_rom.tellg(); rom.resize((size_t)size);
        f_rom.seekg(0); f_rom.read((char*)rom.data(), size);
        if (rom.size() > 0x143) isCgbMode = (rom[0x143] & 0x80) != 0;

        uint8_t code = (rom.size() > 0x147) ? rom[0x147] : 0;
        if (code >= 0x19 && code <= 0x1E) mbcType = 5;
        else if (code >= 0x0F && code <= 0x13) mbcType = 3;
        else if (code >= 0x05 && code <= 0x06) mbcType = 2;
        else if (code >= 0x01 && code <= 0x03) mbcType = 1;
        else mbcType = 0;

        if (isCgbMode) { a=0x11;f=0x80;b=0;c=0;d=0xFF;e=0x56;h=0;l=0x0D; }
        else { a=0x01;f=0xB0;b=0;c=0x13;d=0;e=0xD8;h=1;l=0x4D; }
        sp = 0xFFFE; pc = 0x0100;
        vram.fill(0); vram1.fill(0); oam.fill(0);
        ext_ram.fill(0); wram.fill(0); io.fill(0); hram.fill(0);
        ie=0; ime=false; imeEnablePending=false; halted=false; haltBugPending=false;
        divCounter=0; tima=0; tma=0; tac=0; timaOverflowPending=false; timaOverflowDelay=0;
        romBank=1; ramEnabled=false; ramBank=0; mbcMode=0;
        rtcSelected=false; rtcReg=0; rtcLatchRequested=false;
        for(int i=0;i<5;++i) { rtcRegs[i]=0; rtcLatched[i]=0; }
        rtcLatched_valid = false;
        rtcHalt = false;
        rtcLastUpdate = GetTickCount();
        dmaActive=false; dmaCyclesRemaining=0;
        hdmaSrc=0; hdmaDst=0; hdmaBlocksLeft=0; hdmaActive=false;
		hdmaBank = 0;
		key1Armed=0; cgbDoubleSpeed=false;
        joypSelect=0x30; buttonsState=0xFF;
        lcdc=0x91; stat=0x82; scy=0; scx=0; lyc=0; wy=0; wx=0;
        bgp=0xFC; obp0=0xFF; obp1=0xFF; vbk=0; bcps=0; ocps=0;
// ★ Inicializar paletas a blanco (como el boot ROM de CGB)
for (int i = 0; i < 64; i += 2) {
    bgPaletteRam[i]     = 0xFF;   // byte bajo de blanco (0x7FFF)
    bgPaletteRam[i + 1] = 0x7F;   // byte alto
    objPaletteRam[i]     = 0xFF;
    objPaletteRam[i + 1] = 0x7F;
}
        ppuModeCycles=0; ly=0; windowInternalLine=0;
        apu.reset();
        return true;
    }

    uint8_t vbk = 0;
    uint8_t lcdc=0x91, stat=0x82, scy=0, scx=0, lyc=0, wy=0, wx=0;
    uint8_t bgp=0xFC, obp0=0xFF, obp1=0xFF;
    bool isCgbMode = false;
    std::array<uint8_t, 64> bgPaletteRam{}, objPaletteRam{};
    uint8_t bcps=0, ocps=0;
    int ppuModeCycles = 0;
    int windowInternalLine = 0;

    uint8_t readVram(int bank, uint16_t off) { return bank==0 ? vram[off] : vram1[off]; }

    static uint16_t rgb555(uint8_t lo, uint8_t hi) { return lo | ((uint16_t)hi << 8); }
    static uint32_t rgb555ToArgb(uint16_t c) {
        uint8_t r=c&0x1F,g=(c>>5)&0x1F,b2=(c>>10)&0x1F;
        auto ex=[](uint8_t v){ return (uint8_t)((v<<3)|(v>>2)); };
        return ((uint32_t)ex(r)<<16)|((uint32_t)ex(g)<<8)|(uint32_t)ex(b2);
    }
    static uint32_t dmgShade(uint8_t i) {
        static const uint32_t t[4]={0x00E0F8D0,0x0088C070,0x00346856,0x00081820};
        return t[i&3];
    }

    uint8_t ppuReadReg(uint16_t addr) {
        switch(addr) {
            case 0xFF40: return lcdc;
            case 0xFF41: return stat|0x80;
            case 0xFF42: return scy;
            case 0xFF43: return scx;
            case 0xFF44: return ly;
            case 0xFF45: return lyc;
            case 0xFF47: return bgp;
            case 0xFF48: return obp0;
            case 0xFF49: return obp1;
            case 0xFF4A: return wy;
            case 0xFF4B: return wx;
            case 0xFF4F: return vbk|0xFE;
            case 0xFF68: return bcps|0x40;
            case 0xFF69: return bgPaletteRam[bcps&0x3F];
            case 0xFF6A: return ocps|0x40;
            case 0xFF6B: return objPaletteRam[ocps&0x3F];
        }
        return 0xFF;
    }

    void ppuWriteReg(uint16_t addr, uint8_t val) {
        switch(addr) {
            case 0xFF40: lcdc=val; return;
            case 0xFF41: stat=(stat&0x07)|(val&0xF8); return;
            case 0xFF42: scy=val; return;
            case 0xFF43: scx=val; return;
            case 0xFF44: return;
            case 0xFF45: lyc=val; return;
            case 0xFF46: {
                uint16_t src=(uint16_t)val<<8;
                for(int i=0;i<0xA0;++i) oam[i]=readByte(src+i);
                dmaActive=true; dmaCyclesRemaining=640; return;
            }
            case 0xFF47: bgp=val; return;
            case 0xFF48: obp0=val; return;
            case 0xFF49: obp1=val; return;
            case 0xFF4A: wy=val; return;
            case 0xFF4B: wx=val; return;
            case 0xFF4F: vbk=val&1; return;
            case 0xFF68: bcps=val&0xBF; return;
            case 0xFF69: bgPaletteRam[bcps&0x3F]=val; if(bcps&0x80) bcps=0x80|((bcps+1)&0x3F); return;
            case 0xFF6A: ocps=val&0xBF; return;
            case 0xFF6B: objPaletteRam[ocps&0x3F]=val; if(ocps&0x80) ocps=0x80|((ocps+1)&0x3F); return;
        }
    }

    static constexpr int kOamSearchCycles = 80;
    static constexpr int kPixelTransferCycles = 172;
    static constexpr int kHblankCycles = 456 - kOamSearchCycles - kPixelTransferCycles;

    void stepPpu(int t) {
        if(!(lcdc&0x80)){ly=0;ppuModeCycles=0;stat&=0xFC;windowInternalLine=0;return;}
        ppuModeCycles += t;
        int mode = stat&3;
        if(ly<144) {
            if(mode==2 && ppuModeCycles>=kOamSearchCycles){ppuModeCycles-=kOamSearchCycles;setMode(3);}
            else if(mode==3 && ppuModeCycles>=kPixelTransferCycles){
                ppuModeCycles-=kPixelTransferCycles;renderScanline();setMode(0);

                // ★ HDMA: copiar 16 bytes AL INICIO del HBlank
                if (hdmaActive && hdmaBlocksLeft > 0) {
                    uint16_t dstBase = 0x8000 | (hdmaDst & 0x1FF0);
                    for (int i = 0; i < 16; ++i) {
                        uint8_t b = readByte(hdmaSrc + i);
                        uint16_t addr = dstBase + i;
                        if (addr >= 0xA000) addr -= 0x2000;
                        if (hdmaBank) vram1[addr - 0x8000] = b;
                        else          vram[addr - 0x8000] = b;
                    }
                    hdmaSrc += 16;
                    hdmaDst += 16;
                    hdmaBlocksLeft--;
                    if (hdmaBlocksLeft == 0) {
                        hdmaActive = false;
                        io[0x55] = 0xFF;
                    } else {
                        io[0x55] = (hdmaBlocksLeft - 1) & 0x7F;
                    }
                }
            } else if(mode==0 && ppuModeCycles>=kHblankCycles){
                ppuModeCycles-=kHblankCycles; advanceLine();
                if(ly==144){setMode(1);requestInterrupt(0x01);} else setMode(2);
            }
        } else {
            if(ppuModeCycles>=456){ppuModeCycles-=456;advanceLine();if(ly==0){setMode(2);windowInternalLine=0;}}
        }
    }
    void setMode(int m){stat=(stat&0xFC)|m;bool w=false;if(m==0&&(stat&8))w=true;if(m==2&&(stat&0x20))w=true;if(m==1&&(stat&0x10))w=true;if(w)requestInterrupt(0x02);}
    void advanceLine(){ly++;if(ly>153)ly=0;bool co=(ly==lyc);if(co){stat|=0x04;if(stat&0x40)requestInterrupt(0x02);}else stat&=~0x04;}

    void renderScanline() {
        if(ly>=144) return;
        uint8_t bgi[GB_WIDTH]={0};
        renderBGWindow(bgi);
        if(lcdc&0x02) renderSprites(bgi);
    }

    void renderBGWindow(uint8_t* out) {
        bool bgEn = isCgbMode ? true : (lcdc&0x01)!=0;
        bool winEn = (lcdc&0x20)!=0 && (lcdc&0x01)!=0 && wy<=ly && wx<=166;
        for(int x=0;x<GB_WIDTH;++x){
            bool useWin = winEn && x>=(int(wx)-7);
            uint16_t mapBase = useWin ? ((lcdc&0x40)?0x9C00:0x9800) : ((lcdc&0x08)?0x9C00:0x9800);
            int px = useWin ? (x-(int(wx)-7)) : ((x+scx)&0xFF);
            int py = useWin ? (windowInternalLine&0xFF) : ((ly+scy)&0xFF);
            int col=px/8,row=py/8;
            uint16_t mapAddr = mapBase + row*32 + col;
            uint8_t ti = readVram(0, mapAddr-0x8000);
            uint8_t at = isCgbMode ? readVram(1, mapAddr-0x8000) : 0;
            int tb = isCgbMode ? ((at>>3)&1) : 0;
            bool xf = isCgbMode && (at&0x20);
            bool yf = isCgbMode && (at&0x40);
            bool pri = isCgbMode && (at&0x80);
            int pal = at&7;
            int rowIn = yf ? (7-(py%8)) : (py%8);
            uint16_t td;
            if(lcdc&0x10) td = 0x8000 + (uint16_t)ti*16;
            else td = 0x9000 + (int16_t)(int8_t)ti*16;
            td += rowIn*2;
            uint8_t lo = readVram(tb, td-0x8000);
            uint8_t hi = readVram(tb, td-0x8000+1);
            int bi = xf ? (px%8) : (7-(px%8));
            int ci = (((hi>>bi)&1)<<1) | ((lo>>bi)&1);
            out[x] = (uint8_t)(ci | (pri?0x80:0));
            uint32_t argb;
            if(isCgbMode){
                int po = pal*8 + ci*2;
                argb = rgb555ToArgb(rgb555(bgPaletteRam[po], bgPaletteRam[po+1]));
            } else {
                int sh = (bgp>>(ci*2))&3;
                argb = bgEn ? dmgShade(sh) : dmgShade(0);
            }
            g_screenBuffer[ly*GB_WIDTH+x] = argb;
        }
        if(winEn) windowInternalLine++;
    }

    void renderSprites(uint8_t* bgi) {
        bool tall = (lcdc&0x04)!=0;
        int h = tall?16:8;
        struct E{int idx;uint8_t y,x,t,a;};
        std::vector<E> f;
        for(int i=0;i<40 && f.size()<10;++i){
            uint8_t sy=oam[i*4+0],sx=oam[i*4+1],t=oam[i*4+2],at=oam[i*4+3];
            int top=(int)sy-16;
            if(ly>=top && ly<top+h) f.push_back({i,sy,sx,t,at});
        }
        if(!isCgbMode){
            std::stable_sort(f.begin(),f.end(),[](const E&a2,const E&b2){return a2.x<b2.x;});
        }
        for(auto it=f.rbegin(); it!=f.rend(); ++it){
            const E&s=*it;
            bool xf=s.a&0x20, yf=s.a&0x40;
            bool behind=s.a&0x80;
            int cb=isCgbMode?((s.a>>3)&1):0;
            int cp=s.a&7;
            int dp=(s.a&0x10)?1:0;
            int top=(int)s.y-16;
            int row=ly-top;
            if(yf) row=h-1-row;
            uint8_t ti=tall?(s.t&0xFE):s.t;
            if(tall && row>=8){ti|=1;row-=8;}
            uint16_t td=0x8000+(uint16_t)ti*16+row*2;
            uint8_t lo=readVram(cb,td-0x8000), hi=readVram(cb,td-0x8000+1);
            for(int p=0;p<8;++p){
                int sx2=(int)s.x-8+p;
                if(sx2<0||sx2>=GB_WIDTH) continue;
                int bi=xf?p:(7-p);
                int ci=(((hi>>bi)&1)<<1)|((lo>>bi)&1);
                if(ci==0) continue;
                uint8_t bh=bgi[sx2]&0x3F;
                bool bp=(bgi[sx2]&0x80)!=0;
                if(behind && bh!=0) continue;
                if(isCgbMode && bp && (lcdc&0x01) && bh!=0) continue;
                uint32_t argb;
                if(isCgbMode){int po=cp*8+ci*2;argb=rgb555ToArgb(rgb555(objPaletteRam[po],objPaletteRam[po+1]));}
                else {uint8_t pl=dp?obp1:obp0;int sh=(pl>>(ci*2))&3;argb=dmgShade(sh);}
                g_screenBuffer[ly*GB_WIDTH+sx2]=argb;
            }
        }
    }

    uint8_t readJoypad() const {
        uint8_t r=(joypSelect&0x30)|0xC0;
        uint8_t ln=0x0F;
        if(!(joypSelect&0x10)) ln &= (buttonsState&0x0F);
        if(!(joypSelect&0x20)) ln &= ((buttonsState>>4)&0x0F);
        return r|ln;
    }
    void setButton(int idx, bool p) {
        if(idx<0||idx>7) return;
        uint8_t b=kButtonBit[idx];
        bool wr=((buttonsState>>b)&1)!=0;
        if(p) buttonsState&=~(1u<<b); else buttonsState|=(1u<<b);
        if(p&&wr) requestInterrupt(0x10);
    }
    void releaseAllButtons() { buttonsState=0xFF; }

    // --- MBC1 real bank-select helper ---
    // The 2-bit secondary register ($4000-$5FFF, stored in `ramBank` for
    // MBC1) ALWAYS combines with the 5-bit primary register to form the
    // effective ROM bank for the $4000-$7FFF window, regardless of mode.
    // `mbcMode` only decides whether that same 2-bit register ALSO banks
    // the normally-fixed $0000-$3FFF region and selects the RAM bank
    // (mode 1), or is dedicated purely to ROM bank extension with RAM
    // always on bank 0 and $0000-$3FFF fixed to bank 0 (mode 0).
    uint16_t mbc1EffectiveHighBank() const { return (uint16_t)(romBank & 0x1F) | ((uint16_t)(ramBank & 0x03) << 5); }

    uint8_t readByte(uint16_t addr) {
        if(addr<=0x3FFF) {
            if (mbcType==1 && mbcMode==1) { // mode 1: upper bits also bank the low region
                size_t e=(size_t)((ramBank&0x03)<<5)*0x4000+addr;
                return e<rom.size()?rom[e]:0xFF;
            }
            return addr<rom.size()?rom[addr]:0xFF;
        }
        if(addr<=0x7FFF){
            size_t bank = (mbcType==1) ? mbc1EffectiveHighBank() : romBank;
            size_t e=bank*0x4000+(addr-0x4000);return e<rom.size()?rom[e]:0xFF;
        }
        if(addr>=0x8000&&addr<=0x9FFF){if(dmaActive)return 0xFF;if((stat&3)==3)return 0xFF;return vbk?vram1[addr-0x8000]:vram[addr-0x8000];}
        if(addr>=0xA000&&addr<=0xBFFF){
            if(dmaActive) return 0xFF;
            if(!ramEnabled) return 0xFF;
            if(rtcSelected) {
                // ★ Leer del RTC: si hay latch válido, devolver latched
                uint8_t reg = rtcReg & 0x07;
                if (reg >= 5) return 0xFF;
                if (rtcLatched_valid) return rtcLatched[reg];
                UpdateRtc();
                return rtcRegs[reg];
            }
            uint8_t effectiveRamBank = (mbcType==1 && mbcMode==0) ? 0 : (ramBank & 0x0F);
            return ext_ram[effectiveRamBank*0x2000+(addr-0xA000)];
        }
        if(addr>=0xC000&&addr<=0xDFFF){if(dmaActive)return 0xFF;return wram[addr-0xC000];}
        if(addr>=0xE000&&addr<=0xFDFF) return readByte(addr-0x2000);
        if(addr>=0xFE00&&addr<=0xFE9F){if(dmaActive)return 0xFF;int m=stat&3;if(m==2||m==3)return 0xFF;return oam[addr-0xFE00];}
        if(addr>=0xFEA0&&addr<=0xFEFF) return 0xFF;
        if(addr==0xFF00) return readJoypad();
        if(addr>=0xFF10 && addr<=0xFF3F) {
            // ★ DMG: acceso restringido a wave RAM durante reproducción
            if (addr >= 0xFF30 && addr <= 0xFF3F && !isCgbMode && apu.c3.enabled) {
                uint8_t curByte = (apu.c3.wavePos >> 1) & 15;
                if ((addr - 0xFF30) != curByte) return 0xFF;
            }
            return apu.readReg(addr);
        }
        if(addr==0xFF04) return (uint8_t)(divCounter>>8);
        if(addr==0xFF05) return timaOverflowPending?0x00:tima;
        if(addr==0xFF06) return tma;
        if(addr==0xFF07) return tac|0xF8;
        if(addr==0xFF0F) return (uint8_t)(io[0x0F]|0xE0);
        if(addr==0xFF44) return ly;
        if (addr == 0xFF55) return hdmaActive ? ((hdmaBlocksLeft - 1) & 0x7F) : 0xFF;
        if (addr == 0xFF4D) {
            uint8_t r = 0x7E;
            if (cgbDoubleSpeed) r |= 0x80;
            r |= key1Armed;
            return r;
        }
        if((addr>=0xFF40&&addr<=0xFF4B)||addr==0xFF4F||addr==0xFF68||addr==0xFF69||addr==0xFF6A||addr==0xFF6B) return ppuReadReg(addr);
        if(addr>=0xFF00&&addr<=0xFF7F) return io[addr-0xFF00];
        if(addr>=0xFF80&&addr<=0xFFFE) return hram[addr-0xFF80];
        if(addr==0xFFFF) return (uint8_t)(ie|0xE0);
        return 0xFF;
    }

    void writeByte(uint16_t addr, uint8_t val) {
        if(addr<=0x1FFF){ramEnabled=(val&0x0F)==0x0A;return;}
        if(addr>=0x2000&&addr<=0x3FFF){
            if(mbcType==5){
                if(addr<=0x2FFF) romBank=(romBank&0x100)|(uint16_t)val;
                else romBank=(romBank&0x0FF)|((uint16_t)(val&1)<<8);
            } else if(mbcType==3){
                uint8_t b=val&0x7F; if(b==0)b=1; romBank=b;
            } else {
                uint8_t b=val&0x1F; if(b==0)b=1; romBank=b;
            }
            return;
        }
        if(addr>=0x4000&&addr<=0x5FFF){
            if(mbcType==3){
                if(val<=0x07){ramBank=val; rtcSelected=false;}
                else if(val>=0x08&&val<=0x0C){rtcSelected=true; rtcReg=val-0x08; ramBank=0;}
            } else if(mbcType==5){ramBank=val&0x0F; rtcSelected=false;}
            else {ramBank=val&0x03; rtcSelected=false;}
            return;
        }
        if(addr>=0x6000&&addr<=0x7FFF){
            if(mbcType==3){
                if(val==0x00) rtcLatchRequested=true;
                else if(val==0x01 && rtcLatchRequested){
                    rtcLatchRequested=false;
                    // ★ Latch: copiar valores vivos a latched
                    UpdateRtc();
                    for(int i=0;i<5;++i) rtcLatched[i] = rtcRegs[i];
                    rtcLatched_valid = true;
                }
            }
            mbcMode=val&0x01;
            return;
        }
        if(addr>=0x8000&&addr<=0x9FFF){
            if(dmaActive) return;
            if((stat&3)==3) return;
            if(vbk) vram1[addr-0x8000]=val; else vram[addr-0x8000]=val;
            return;
        }
        if(addr>=0xA000&&addr<=0xBFFF){
            if(dmaActive) return;
            if(!ramEnabled) return;
            if(rtcSelected) {
                // ★ Escribir al RTC (register $08-$0C)
                UpdateRtc();
                uint8_t reg = rtcReg & 0x07;
                if (reg < 5) {
                    if (reg == 4) {
                        // DH: bit 6 = halt, bit 0 = day high
                        rtcHalt = (val & 0x40) != 0;
                        rtcRegs[4] = (rtcRegs[4] & 0xBE) | (val & 0x41);
                    } else {
                        rtcRegs[reg] = val;
                    }
                }
                return;
            }
            uint8_t effectiveRamBank = (mbcType==1 && mbcMode==0) ? 0 : (ramBank & 0x0F);
            ext_ram[effectiveRamBank*0x2000+(addr-0xA000)]=val;
            return;
        }
		if(addr>=0xC000&&addr<=0xDFFF){if(dmaActive)return;wram[addr-0xC000]=val;return;}
        if(addr>=0xE000&&addr<=0xFDFF){writeByte(addr-0x2000,val);return;}
        if(addr>=0xFEA0&&addr<=0xFEFF) return;
        if(addr==0xFF01){io[1]=val;return;}
        if(addr==0xFF02){io[2]=val;if(val&0x80){io[2]&=0x7F;requestInterrupt(0x08);}return;}
        if(addr>=0xFF10 && addr<=0xFF3F){
            // ★ DMG: acceso restringido a wave RAM durante reproducción
            if (addr >= 0xFF30 && addr <= 0xFF3F && !isCgbMode && apu.c3.enabled) {
                uint8_t curByte = (apu.c3.wavePos >> 1) & 15;
                if ((addr - 0xFF30) != curByte) return;
            }
            apu.writeReg(addr,val);
            return;
        }
        if(addr==0xFF04){bool o=timerInputSignal();divCounter=0;bool n=timerInputSignal();if(o&&!n)incrementTimerOnFallingEdge();return;}
        if(addr==0xFF05){if(timaOverflowPending){timaOverflowPending=false;timaOverflowDelay=0;}tima=val;return;}
        if(addr==0xFF06){tma=val;return;}
        if(addr==0xFF07){bool o=timerInputSignal();tac=val&0x07;bool n=timerInputSignal();if(o&&!n)incrementTimerOnFallingEdge();return;}
        if(addr==0xFF0F){io[0x0F]=val&0x1F;return;}
        if(addr==0xFF44) return;
        // ★ HDMA registers
        if (addr == 0xFF51) { hdmaSrc = (hdmaSrc & 0x00FF) | (val << 8); return; }
        if (addr == 0xFF4D) {
            if (isCgbMode) key1Armed = val & 1;
            return;
        }
        if (addr == 0xFF52) { hdmaSrc = (hdmaSrc & 0xFF00) | (val & 0xF0); return; }
        if (addr == 0xFF53) { hdmaDst = (hdmaDst & 0x00FF) | ((val & 0x1F) << 8); hdmaDst |= 0x8000; return; }
        if (addr == 0xFF54) { hdmaDst = (hdmaDst & 0xFF00) | (val & 0xF0); return; }
        if (addr == 0xFF55) {
            if (!(val & 0x80)) {
                // GDMA o cancelar HDMA
                if (hdmaActive) {
                    // Cancelar HDMA: bit 7 = 1 al leer, lower 7 = bloques restantes - 1
                    hdmaActive = false;
                    io[0x55] = 0x80 | ((hdmaBlocksLeft - 1) & 0x7F);
                } else {
                    // GDMA: copiar todo de una vez
                    int blocks = (val & 0x7F) + 1;
                    uint16_t dstBase = 0x8000 | (hdmaDst & 0x1FF0);
                    uint8_t bank = vbk;   // ★ latch al iniciar
                    for (int i = 0; i < blocks * 16; ++i) {
                        uint8_t b = readByte(hdmaSrc + i);
                        uint16_t a = dstBase + i;
                        if (a >= 0xA000) a -= 0x2000;   // wrap dentro de VRAM
                        if (bank) vram1[a - 0x8000] = b;
                        else      vram[a - 0x8000] = b;
                    }
                    io[0x55] = 0xFF;
                }
            } else {
                // HDMA: iniciar (si ya está activa, ignorar)
                if (!hdmaActive) {
                    hdmaBank = vbk;   // ★ latch del banco
                    hdmaActive = true;
                    hdmaBlocksLeft = (val & 0x7F) + 1;
                    io[0x55] = (hdmaBlocksLeft - 1) & 0x7F;
                }
            }
            return;
        }
        if((addr>=0xFF40&&addr<=0xFF46)||(addr>=0xFF47&&addr<=0xFF4B)||addr==0xFF4F||addr==0xFF68||addr==0xFF69||addr==0xFF6A||addr==0xFF6B){ppuWriteReg(addr,val);return;}
		if(addr==0xFF00){joypSelect=val&0x30;return;}
        if(addr>=0xFF00&&addr<=0xFF7F){io[addr-0xFF00]=val;return;}
        if(addr>=0xFF80&&addr<=0xFFFE){hram[addr-0xFF80]=val;return;}
        if(addr==0xFFFF){ie=val&0x1F;return;}
    }

    void requestInterrupt(uint8_t bit){io[0x0F]|=bit;}
    void setFlag(uint8_t m,bool v){if(v)f|=m;else f&=~m;f&=0xF0;}
    bool getFlag(uint8_t m) const {return (f&m)!=0;}
    uint16_t bc() const {return ((uint16_t)b<<8)|c;}
    uint16_t de() const {return ((uint16_t)d<<8)|e;}
    uint16_t hl() const {return ((uint16_t)h<<8)|l;}
    void setBc(uint16_t v){b=v>>8;c=v&0xFF;}
    void setDe(uint16_t v){d=v>>8;e=v&0xFF;}
    void setHl(uint16_t v){h=v>>8;l=v&0xFF;}

    uint8_t getR(int i){switch(i){case 0:return b;case 1:return c;case 2:return d;case 3:return e;case 4:return h;case 5:return l;case 6:return readByte(hl());default:return a;}}
    void setR(int i,uint8_t v){switch(i){case 0:b=v;break;case 1:c=v;break;case 2:d=v;break;case 3:e=v;break;case 4:h=v;break;case 5:l=v;break;case 6:writeByte(hl(),v);break;default:a=v;break;}}

    uint8_t fetch8(){uint8_t v=readByte(pc);if(haltBugPending)haltBugPending=false;else pc++;return v;}
    uint16_t fetch16(){uint8_t lo=fetch8(),hi=fetch8();return ((uint16_t)hi<<8)|lo;}
    void push16(uint16_t v){sp-=2;writeByte(sp,v&0xFF);writeByte(sp+1,v>>8);}
    uint16_t pop16(){uint16_t v=readByte(sp)|((uint16_t)readByte(sp+1)<<8);sp+=2;return v;}

    void aluAdd(uint8_t v,bool c){int cy=(c&&getFlag(FLAG_C))?1:0;int r=a+v+cy;setFlag(FLAG_H,((a&0xF)+(v&0xF)+cy)>0xF);setFlag(FLAG_C,r>0xFF);a=(uint8_t)r;setFlag(FLAG_Z,a==0);setFlag(FLAG_N,false);}
    void aluSub(uint8_t v,bool c,bool s){int cy=(c&&getFlag(FLAG_C))?1:0;int r=a-v-cy;setFlag(FLAG_H,((a&0xF)-(v&0xF)-cy)<0);setFlag(FLAG_C,r<0);uint8_t r8=(uint8_t)r;setFlag(FLAG_Z,r8==0);setFlag(FLAG_N,true);if(s)a=r8;}
    void aluAnd(uint8_t v){a&=v;setFlag(FLAG_Z,a==0);setFlag(FLAG_N,false);setFlag(FLAG_H,true);setFlag(FLAG_C,false);}
    void aluXor(uint8_t v){a^=v;setFlag(FLAG_Z,a==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);setFlag(FLAG_C,false);}
    void aluOr(uint8_t v){a|=v;setFlag(FLAG_Z,a==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);setFlag(FLAG_C,false);}
    uint8_t incVal(uint8_t v){setFlag(FLAG_H,(v&0xF)==0xF);v++;setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);return v;}
    uint8_t decVal(uint8_t v){setFlag(FLAG_H,(v&0xF)==0);v--;setFlag(FLAG_Z,v==0);setFlag(FLAG_N,true);return v;}
    uint8_t rlc(uint8_t v){bool c2=v&0x80;v=(v<<1)|(c2?1:0);setFlag(FLAG_C,c2);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t rrc(uint8_t v){bool c2=v&1;v=(v>>1)|(c2?0x80:0);setFlag(FLAG_C,c2);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t rl(uint8_t v){bool oc=getFlag(FLAG_C);bool nc=v&0x80;v=(v<<1)|(oc?1:0);setFlag(FLAG_C,nc);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t rr(uint8_t v){bool oc=getFlag(FLAG_C);bool nc=v&1;v=(v>>1)|(oc?0x80:0);setFlag(FLAG_C,nc);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t sla(uint8_t v){bool c2=v&0x80;v<<=1;setFlag(FLAG_C,c2);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t sra(uint8_t v){bool c2=v&1;v=(v>>1)|(v&0x80);setFlag(FLAG_C,c2);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t srl(uint8_t v){bool c2=v&1;v>>=1;setFlag(FLAG_C,c2);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);return v;}
    uint8_t swap(uint8_t v){v=(v<<4)|(v>>4);setFlag(FLAG_Z,v==0);setFlag(FLAG_N,false);setFlag(FLAG_H,false);setFlag(FLAG_C,false);return v;}

    static constexpr std::array<uint16_t,5> kVectors={0x0040,0x0048,0x0050,0x0058,0x0060};

    int dispatchInterrupt(uint8_t p){
        for(int b=0;b<5;++b){
            if(p&(1u<<b)){
                ime=false;io[0x0F]&=~(1u<<b);push16(pc);pc=kVectors[b];return 20;
            }
        }
        return 0;
    }

    bool timerInputSignal() const {if(!(tac&0x04))return false;return ((divCounter>>kClockSelectBit[tac&3])&1)!=0;}
    void incrementTimerOnFallingEdge(){if(timaOverflowPending)return;uint8_t o=tima;tima=(uint8_t)(tima+1);if(o==0xFF){tima=0;timaOverflowPending=true;timaOverflowDelay=4;}}
    void stepTimer(int t){
        if (t <= 0) return;

        // 1) Procesar overflow pendiente de TIMA
        if (timaOverflowPending) {
            if (t <= timaOverflowDelay) {
                timaOverflowDelay -= t;
                divCounter += (uint16_t)t;
                return;
            }
            // Avanzar DIV por el delay consumido
            divCounter += (uint16_t)timaOverflowDelay;
            t -= timaOverflowDelay;
            timaOverflowDelay = 0;
            tima = tma;
            requestInterrupt(0x04);
            timaOverflowPending = false;
        }

        // 2) Timer desactivado: solo avanzar DIV
        if (!(tac & 0x04)) {
            divCounter += (uint16_t)t;
            return;
        }

        // 3) Calcular cuántos flancos caen en los t ciclos
        int bit = kClockSelectBit[tac & 3];
        int period = 1 << bit;
        int phase = divCounter & (period - 1);
        int firstEdge = period - phase;

        if (t < firstEdge) {
            divCounter += (uint16_t)t;
            return;
        }

        // Primera caída
        divCounter += (uint16_t)firstEdge;
        t -= firstEdge;
        {
            uint8_t oldTima = tima;
            tima = (uint8_t)(tima + 1);
            if (oldTima == 0xFF) {
                tima = 0;
                timaOverflowPending = true;
                timaOverflowDelay = 4;
                divCounter += (uint16_t)(t % period);
                return;
            }
        }

        // Caídas siguientes
        int edges = t / period;
        int remainder = t % period;

        for (int e = 0; e < edges; ++e) {
            uint8_t oldTima = tima;
            tima = (uint8_t)(tima + 1);
            if (oldTima == 0xFF) {
                tima = 0;
                timaOverflowPending = true;
                timaOverflowDelay = 4;
                divCounter += (uint16_t)remainder;
                return;
            }
        }

        divCounter += (uint16_t)remainder;
    }
    void tickDma(int t){if(!dmaActive)return;dmaCyclesRemaining-=t;if(dmaCyclesRemaining<=0){dmaActive=false;dmaCyclesRemaining=0;}}

    int step() {
        if (rom.empty()) return 0;

        // En double-speed, CPU/Timer/OAM-DMA corren 2×; PPU/HDMA/APU a velocidad normal
        int speedMult = cgbDoubleSpeed ? 2 : 1;

        if (halted) {
            uint8_t p = ie & io[0x0F] & 0x1F;
            if (p) halted = false;
            stepTimer(4 * speedMult);
            stepPpu(4);
            tickDma(4 * speedMult);
            return 4;
        }

        if (ime) {
            uint8_t p = ie & io[0x0F] & 0x1F;
            if (p) {
                int t = dispatchInterrupt(p);          // T-cycles reales de la instrucción
                int realDots = t / speedMult;          // dots reales consumidos
                if (realDots < 1) realDots = 1;
                stepTimer(realDots * speedMult);       // timer va 2× en double-speed
                stepPpu(realDots);                     // PPU a velocidad normal
                tickDma(realDots * speedMult);         // OAM DMA va 2× en double-speed
                return realDots;
            }
        }

        bool en = imeEnablePending;
        uint8_t op = fetch8();
        int t = execute(op);

        int realDots = t / speedMult;
        if (realDots < 1) realDots = 1;

        stepTimer(realDots * speedMult);
        stepPpu(realDots);
        tickDma(realDots * speedMult);

        if (en) { ime = true; imeEnablePending = false; }
        return realDots;
    }

    int execute(uint8_t op) {
        if(op>=0x40&&op<=0x7F&&op!=0x76){int d=(op>>3)&7,s=op&7;setR(d,getR(s));return (d==6||s==6)?8:4;}
        if(op==0x76){bool p=(ie&readByte(0xFF0F)&0x1F)!=0;if(!ime&&p){haltBugPending=true;halted=false;}else halted=true;return 4;}
        if(op>=0x80&&op<=0xBF){int ao=(op>>3)&7,s=op&7;uint8_t v=getR(s);switch(ao){case 0:aluAdd(v,false);break;case 1:aluAdd(v,true);break;case 2:aluSub(v,false,true);break;case 3:aluSub(v,true,true);break;case 4:aluAnd(v);break;case 5:aluXor(v);break;case 6:aluOr(v);break;case 7:aluSub(v,false,false);break;}return s==6?8:4;}
        if((op&0xC7)==0x06){int d=(op>>3)&7;uint8_t n=fetch8();setR(d,n);return d==6?12:8;}
        if((op&0xC7)==0x04){int r=(op>>3)&7;setR(r,incVal(getR(r)));return r==6?12:4;}
        if((op&0xC7)==0x05){int r=(op>>3)&7;setR(r,decVal(getR(r)));return r==6?12:4;}
        switch(op){
            case 0x00:return 4;
            case 0x10: {
                if (isCgbMode && (key1Armed & 1)) {
                    cgbDoubleSpeed = !cgbDoubleSpeed;
                    key1Armed = 0;
                    fetch8();
                    // El pause es siempre 8200 dots (2050 M-cycles × 4 T-cycles)
                    // En double-speed cada dot dura la mitad, pero el hardware
                    // mantiene el pause en 8200 dots = 4100 "fast M-cycles"
                    return cgbDoubleSpeed ? 4100 : 8200;
                }
                fetch8();
                return 4;
            }
            case 0xF3:ime=false;imeEnablePending=false;return 4;
            case 0xFB:if(!ime)imeEnablePending=true;return 4;
            case 0x27:{int adj=0;bool setC=getFlag(FLAG_C);if(getFlag(FLAG_N)){if(getFlag(FLAG_H))adj+=0x06;if(getFlag(FLAG_C))adj+=0x60;a=(uint8_t)(a-adj);}else{if(getFlag(FLAG_H)||(a&0xF)>9)adj+=0x06;if(getFlag(FLAG_C)||a>0x99){adj+=0x60;setC=true;}a=(uint8_t)(a+adj);}setFlag(FLAG_Z,a==0);setFlag(FLAG_H,false);setFlag(FLAG_C,setC);return 4;}
            case 0x2F:a=~a;setFlag(FLAG_N,true);setFlag(FLAG_H,true);return 4;
            case 0x37:setFlag(FLAG_N,false);setFlag(FLAG_H,false);setFlag(FLAG_C,true);return 4;
            case 0x3F:setFlag(FLAG_N,false);setFlag(FLAG_H,false);setFlag(FLAG_C,!getFlag(FLAG_C));return 4;
            case 0x07:a=rlc(a);setFlag(FLAG_Z,false);return 4;
            case 0x0F:a=rrc(a);setFlag(FLAG_Z,false);return 4;
            case 0x17:a=rl(a);setFlag(FLAG_Z,false);return 4;
            case 0x1F:a=rr(a);setFlag(FLAG_Z,false);return 4;
            case 0x01:setBc(fetch16());return 12;
            case 0x11:setDe(fetch16());return 12;
            case 0x21:setHl(fetch16());return 12;
            case 0x31:sp=fetch16();return 12;
            case 0x08:{uint16_t addr=fetch16();writeByte(addr,sp&0xFF);writeByte(addr+1,sp>>8);return 20;}
            case 0xF9:sp=hl();return 8;
            case 0xF8:{int8_t d=(int8_t)fetch8();int r=sp+d;setFlag(FLAG_Z,false);setFlag(FLAG_N,false);setFlag(FLAG_H,((sp&0xF)+(d&0xF))>0xF);setFlag(FLAG_C,((sp&0xFF)+(uint8_t)d)>0xFF);setHl((uint16_t)r);return 12;}
            case 0xE8:{int8_t d=(int8_t)fetch8();int r=sp+d;setFlag(FLAG_Z,false);setFlag(FLAG_N,false);setFlag(FLAG_H,((sp&0xF)+(d&0xF))>0xF);setFlag(FLAG_C,((sp&0xFF)+(uint8_t)d)>0xFF);sp=(uint16_t)r;return 16;}
            case 0x03:setBc(bc()+1);return 8;case 0x13:setDe(de()+1);return 8;case 0x23:setHl(hl()+1);return 8;case 0x33:sp++;return 8;
            case 0x0B:setBc(bc()-1);return 8;case 0x1B:setDe(de()-1);return 8;case 0x2B:setHl(hl()-1);return 8;case 0x3B:sp--;return 8;
            case 0x09:case 0x19:case 0x29:case 0x39:{uint16_t v=op==0x09?bc():op==0x19?de():op==0x29?hl():sp;int r=hl()+v;setFlag(FLAG_N,false);setFlag(FLAG_H,((hl()&0xFFF)+(v&0xFFF))>0xFFF);setFlag(FLAG_C,r>0xFFFF);setHl((uint16_t)r);return 8;}
            case 0x02:writeByte(bc(),a);return 8;case 0x12:writeByte(de(),a);return 8;
            case 0x22:writeByte(hl(),a);setHl(hl()+1);return 8;case 0x32:writeByte(hl(),a);setHl(hl()-1);return 8;
            case 0x0A:a=readByte(bc());return 8;case 0x1A:a=readByte(de());return 8;
            case 0x2A:a=readByte(hl());setHl(hl()+1);return 8;case 0x3A:a=readByte(hl());setHl(hl()-1);return 8;
            case 0xC6:aluAdd(fetch8(),false);return 8;case 0xCE:aluAdd(fetch8(),true);return 8;
            case 0xD6:aluSub(fetch8(),false,true);return 8;case 0xDE:aluSub(fetch8(),true,true);return 8;
            case 0xE6:aluAnd(fetch8());return 8;case 0xEE:aluXor(fetch8());return 8;
            case 0xF6:aluOr(fetch8());return 8;case 0xFE:aluSub(fetch8(),false,false);return 8;
            case 0xC3:pc=fetch16();return 16;
            case 0xE9:pc=hl();return 4;
            case 0x18:{int8_t d=(int8_t)fetch8();pc=(uint16_t)(pc+d);return 12;}
            case 0xC2:case 0xCA:case 0xD2:case 0xDA:{uint16_t addr=fetch16();bool c=op==0xC2?!getFlag(FLAG_Z):op==0xCA?getFlag(FLAG_Z):op==0xD2?!getFlag(FLAG_C):getFlag(FLAG_C);if(c){pc=addr;return 16;}return 12;}
            case 0x20:case 0x28:case 0x30:case 0x38:{int8_t d=(int8_t)fetch8();bool c=op==0x20?!getFlag(FLAG_Z):op==0x28?getFlag(FLAG_Z):op==0x30?!getFlag(FLAG_C):getFlag(FLAG_C);if(c){pc=(uint16_t)(pc+d);return 12;}return 8;}
            case 0xCD:{uint16_t addr=fetch16();push16(pc);pc=addr;return 24;}
            case 0xC4:case 0xCC:case 0xD4:case 0xDC:{uint16_t addr=fetch16();bool c=op==0xC4?!getFlag(FLAG_Z):op==0xCC?getFlag(FLAG_Z):op==0xD4?!getFlag(FLAG_C):getFlag(FLAG_C);if(c){push16(pc);pc=addr;return 24;}return 12;}
            case 0xC9:pc=pop16();return 16;
            case 0xD9:pc=pop16();ime=true;imeEnablePending=false;return 16;
            case 0xC0:case 0xC8:case 0xD0:case 0xD8:{bool c=op==0xC0?!getFlag(FLAG_Z):op==0xC8?getFlag(FLAG_Z):op==0xD0?!getFlag(FLAG_C):getFlag(FLAG_C);if(c){pc=pop16();return 20;}return 8;}
            case 0xC7:case 0xCF:case 0xD7:case 0xDF:case 0xE7:case 0xEF:case 0xF7:case 0xFF:push16(pc);pc=op-0xC7;return 16;
            case 0xC5:push16(bc());return 16;case 0xD5:push16(de());return 16;
            case 0xE5:push16(hl());return 16;case 0xF5:push16(((uint16_t)a<<8)|f);return 16;
            case 0xC1:setBc(pop16());return 12;case 0xD1:setDe(pop16());return 12;
            case 0xE1:setHl(pop16());return 12;case 0xF1:{uint16_t v=pop16();a=v>>8;f=v&0xF0;return 12;}
            case 0xE0:writeByte(0xFF00+fetch8(),a);return 12;
            case 0xF0:a=readByte(0xFF00+fetch8());return 12;
            case 0xE2:writeByte(0xFF00+c,a);return 8;
            case 0xF2:a=readByte(0xFF00+c);return 8;
            case 0xEA:writeByte(fetch16(),a);return 16;
            case 0xFA:a=readByte(fetch16());return 16;
            case 0xCB:return 4+executeCb(fetch8());
            default:return 4;
        }
    }

    int executeCb(uint8_t op){
        int r=op&7,b=(op>>3)&7;uint8_t v=getR(r),res=v;int g=op>>6;
        if(g==0){switch(b){case 0:res=rlc(v);break;case 1:res=rrc(v);break;case 2:res=rl(v);break;case 3:res=rr(v);break;case 4:res=sla(v);break;case 5:res=sra(v);break;case 6:res=swap(v);break;case 7:res=srl(v);break;}setR(r,res);}
        else if(g==1){setFlag(FLAG_Z,((v>>b)&1)==0);setFlag(FLAG_N,false);setFlag(FLAG_H,true);}
        else if(g==2){setR(r,v&~(1u<<b));}
        else {setR(r,v|(1u<<b));}
        return r==6?((g==1)?8:12):4;
    }
};
Bus g_bus;

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
    if (!g_waveOutReady || !g_audio_enabled) return;

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
    for (int a = 0; a < 8; ++a)
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
}

void CargarConfiguracionIni() {
    char szPath[MAX_PATH]; GetIniPath(szPath);
    for (int a = 0; a < 8; ++a)
        for (int c = 0; c < 4; ++c) {
            char key[32];
            wsprintfA(key, "%s_%d", g_action_names[a], c + 1);
            g_bindings[a][c] = GetPrivateProfileIntA("Keyboard", key, g_bindings[a][c], szPath);
        }
    g_window_scale = GetPrivateProfileIntA("Video", "Scale", 2, szPath);
    if (g_window_scale < 1) g_window_scale = 1;
    if (g_window_scale > 4) g_window_scale = 4;
    g_audio_enabled = GetPrivateProfileIntA("Audio", "Enabled", 1, szPath) != 0;
}

void GuardarSramCartucho() {
    if (!g_rom_loaded) return;
    CreateDirectoryA(".\\gbsave", NULL);
    char szPath[MAX_PATH];
    wsprintfA(szPath, ".\\gbsave\\%s.sav", g_rom_base_name);
    std::ofstream f(szPath, std::ios::binary);
    if (!f) return;
    f.write((char*)g_bus.ext_ram.data(), g_bus.ext_ram.size());
}

void CargarSramCartucho() {
    if (!g_rom_loaded) return;
    char szPath[MAX_PATH];
    wsprintfA(szPath, ".\\gbsave\\%s.sav", g_rom_base_name);
    std::ifstream f(szPath, std::ios::binary | std::ios::ate);
    if (!f) return;   // no existe: arranca con SRAM a cero (juego nuevo)
    auto fsize = f.tellg();
    if (fsize <= 0) return;
    size_t toRead = (size_t)fsize;
    if (toRead > g_bus.ext_ram.size()) toRead = g_bus.ext_ram.size();
    f.seekg(0);
    f.read((char*)g_bus.ext_ram.data(), toRead);
}

void GetKeyNameStr(DWORD vk, char* outName) {
    if (vk == 0) { lstrcpyA(outName, "???"); return; }
    if (vk == VK_SPACE) lstrcpyA(outName, "Space");
    else if (vk == VK_RETURN) lstrcpyA(outName, "Enter");
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
static const int kAllCtrlIds[8][4] = {
    { IDC_KEY_UP_1,     IDC_KEY_UP_2,     IDC_KEY_UP_3,     IDC_KEY_UP_4     },
    { IDC_KEY_DOWN_1,   IDC_KEY_DOWN_2,   IDC_KEY_DOWN_3,   IDC_KEY_DOWN_4   },
    { IDC_KEY_LEFT_1,   IDC_KEY_LEFT_2,   IDC_KEY_LEFT_3,   IDC_KEY_LEFT_4   },
    { IDC_KEY_RIGHT_1,  IDC_KEY_RIGHT_2,  IDC_KEY_RIGHT_3,  IDC_KEY_RIGHT_4  },
    { IDC_KEY_A_1,      IDC_KEY_A_2,      IDC_KEY_A_3,      IDC_KEY_A_4      },
    { IDC_KEY_B_1,      IDC_KEY_B_2,      IDC_KEY_B_3,      IDC_KEY_B_4      },
    { IDC_KEY_SELECT_1, IDC_KEY_SELECT_2, IDC_KEY_SELECT_3, IDC_KEY_SELECT_4 },
    { IDC_KEY_START_1,  IDC_KEY_START_2,  IDC_KEY_START_3,  IDC_KEY_START_4  }
};

static bool BuscarIndiceBinding(int ctrlId, int& outA, int& outC) {
    for (int a = 0; a < 8; ++a)
        for (int c = 0; c < 4; ++c)
            if (kAllCtrlIds[a][c] == ctrlId) { outA = a; outC = c; return true; }
    return false;
}

// Get the 4 column IDs for a given action
int GetColumnID(int action, int col) {
    // rows: 0=UP 1=DOWN 2=LEFT 3=RIGHT 4=A 5=B 6=AutoA 7=AutoB 8=Start 9=Select
    static const int bases[10] = {
        IDC_KEY_UP_1, IDC_KEY_DOWN_1, IDC_KEY_LEFT_1, IDC_KEY_RIGHT_1,
        IDC_KEY_A_1, IDC_KEY_B_1, IDC_KEY_AUTOA_1, IDC_KEY_AUTOB_1,
        IDC_KEY_START_1, IDC_KEY_SELECT_1
    };
    return bases[action] + col;
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
        // TAB: dejar pasar al WndProc original (que mueve el foco)
        if (wp == VK_TAB) {
            if (a >= 0 && c >= 0 && g_origEditProc[a][c]) {
                return CallWindowProcA(g_origEditProc[a][c], hWnd, msg, wp, lp);
            }
            return DefWindowProcA(hWnd, msg, wp, lp);
        }
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

        for (int a = 0; a < 8; ++a) {
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
            for (int a = 0; a < 8; ++a)
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
            for (int a = 0; a < 8; ++a)
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
    RECT rc = {0, 0, GB_WIDTH * scale, GB_HEIGHT * scale};
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

void LiberarRomActual(HWND hWnd) {
    if (!g_rom_loaded) return;
    g_bus.rom.clear();
    g_rom_loaded = false;
    g_emulation_paused = false;
    for (int i = 0; i < GB_WIDTH * GB_HEIGHT; ++i) g_screenBuffer[i] = 0;
    ActualizarEstadoMenusArchivo(hWnd);
    InvalidateRect(hWnd, NULL, TRUE);
    UpdateWindow(hWnd);
    MessageBoxA(hWnd, "ROM liberada de la memoria RAM con exito.", "Rew. Reborn", MB_OK | MB_ICONINFORMATION);
}

void ResetearEmulador(HWND hWnd) {
    if (!g_rom_loaded) return;

    // ─── CPU: valores de boot según modo CGB o DMG ───────────────────
    if (g_bus.isCgbMode) {
        // Boot real de Game Boy Color
        g_bus.a = 0x11; g_bus.f = 0x80;
        g_bus.b = 0x00; g_bus.c = 0x00;
        g_bus.d = 0xFF; g_bus.e = 0x56;
        g_bus.h = 0x00; g_bus.l = 0x0D;
    } else {
        // Boot real de Game Boy / Pocket
        g_bus.a = 0x01; g_bus.f = 0xB0;
        g_bus.b = 0x00; g_bus.c = 0x13;
        g_bus.d = 0x00; g_bus.e = 0xD8;
        g_bus.h = 0x01; g_bus.l = 0x4D;
    }
    g_bus.pc = 0x100; g_bus.sp = 0xFFFE;
    g_bus.ime = false; g_bus.imeEnablePending = false;
    g_bus.halted = false; g_bus.haltBugPending = false;

    // ─── Timer ───────────────────────────────────────────────────────
    g_bus.divCounter = 0; g_bus.tima = 0; g_bus.tma = 0; g_bus.tac = 0;
    g_bus.timaOverflowPending = false; g_bus.timaOverflowDelay = 0;

    // ─── MBC ─────────────────────────────────────────────────────────
    g_bus.ramEnabled = false; g_bus.ramBank = 0; g_bus.mbcMode = 0;
    g_bus.rtcSelected = false; g_bus.rtcReg = 0; g_bus.rtcLatchRequested = false;
    for (int i = 0; i < 5; ++i) { g_bus.rtcRegs[i] = 0; g_bus.rtcLatched[i] = 0; }
    g_bus.rtcLatched_valid = false;
    g_bus.rtcHalt = false;
    g_bus.rtcLastUpdate = GetTickCount();
    g_bus.romBank = 1;

    // ─── DMA ─────────────────────────────────────────────────────────
    g_bus.dmaActive = false; g_bus.dmaCyclesRemaining = 0;

    // ─── Joypad ──────────────────────────────────────────────────────
    g_bus.joypSelect = 0x30; g_bus.buttonsState = 0xFF;
    // ─── Memoria (NO tocar ext_ram: ahí vive el save del cartucho) ──
    g_bus.vram.fill(0);
    g_bus.vram1.fill(0);
    g_bus.oam.fill(0);
    g_bus.io.fill(0);
    g_bus.hram.fill(0);
    g_bus.ie = 0;

    // ─── PPU ─────────────────────────────────────────────────────────
    g_bus.ly = 0; g_bus.ppuModeCycles = 0; g_bus.windowInternalLine = 0;
    g_bus.lcdc = 0x91; g_bus.stat = 0x82;
    g_bus.scy = 0; g_bus.scx = 0; g_bus.lyc = 0;
    g_bus.wy = 0; g_bus.wx = 0;
    g_bus.bgp = 0xFC; g_bus.obp0 = 0xFF; g_bus.obp1 = 0xFF;

    // ─── Paletas CGB: limpiar (el juego las re-inicializa en boot) ──
    g_bus.vbk = 0; g_bus.bcps = 0; g_bus.ocps = 0;
// ★ Inicializar paletas a blanco (como el boot ROM de CGB)
for (int i = 0; i < 64; i += 2) {
    g_bus.bgPaletteRam[i]     = 0xFF;
    g_bus.bgPaletteRam[i + 1] = 0x7F;
    g_bus.objPaletteRam[i]     = 0xFF;
    g_bus.objPaletteRam[i + 1] = 0x7F;
}

    // ─── APU ─────────────────────────────────────────────────────────
	
    g_bus.apu.reset();
	g_bus.hdmaBank = 0;
	g_bus.key1Armed = 0;
    g_bus.cgbDoubleSpeed = false;

    // ─── Pantalla a verde DMG ────────────────────────────────────────
    for (int i = 0; i < GB_WIDTH * GB_HEIGHT; ++i)
        g_screenBuffer[i] = 0x00557755;

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

void OpenRomDialog(HWND hWnd) {
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Game Boy ROMs\0*.gb;*.gbc\0";
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn) && g_bus.loadRom(std::string(szFile))) {
        g_rom_loaded = true;
        g_bus.romBank = 1;
        char* file_name = strrchr(szFile, '\\');
        if (file_name) file_name++;
        else file_name = szFile;
        lstrcpyA(g_rom_base_name, file_name);
        char* dot = strrchr(g_rom_base_name, '.');
        if (dot) *dot = '\0';
		CargarSramCartucho();
        for (int i = 0; i < GB_WIDTH * GB_HEIGHT; ++i) g_screenBuffer[i] = 0x00557755;
        ActualizarEstadoMenusArchivo(hWnd);
        RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
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
bool saveStateToFile(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t magic = Bus::kStateMagic, version = Bus::kStateVersion;
    f.write((char*)&magic, 4); f.write((char*)&version, 4);
    f.write((char*)&g_bus.pc, 2); f.write((char*)&g_bus.sp, 2);
    f.write((char*)&g_bus.a, 1); f.write((char*)&g_bus.f, 1);
    f.write((char*)&g_bus.b, 1); f.write((char*)&g_bus.c, 1);
    f.write((char*)&g_bus.d, 1); f.write((char*)&g_bus.e, 1);
    f.write((char*)&g_bus.h, 1); f.write((char*)&g_bus.l, 1);
    uint8_t bi=g_bus.ime,bie=g_bus.imeEnablePending,bh=g_bus.halted,bhp=g_bus.haltBugPending;
    f.write((char*)&bi, 1); f.write((char*)&bie, 1);
    f.write((char*)&bh, 1); f.write((char*)&bhp, 1);
    f.write((char*)&g_bus.divCounter, 2);
    f.write((char*)&g_bus.tima, 1); f.write((char*)&g_bus.tma, 1); f.write((char*)&g_bus.tac, 1);
    uint8_t btop = g_bus.timaOverflowPending; f.write((char*)&btop, 1);
    int32_t tdel = g_bus.timaOverflowDelay; f.write((char*)&tdel, 4);
    f.write((char*)&g_bus.lcdc, 1); f.write((char*)&g_bus.stat, 1);
    f.write((char*)&g_bus.scy, 1); f.write((char*)&g_bus.scx, 1);
    f.write((char*)&g_bus.lyc, 1); f.write((char*)&g_bus.wy, 1); f.write((char*)&g_bus.wx, 1);
    f.write((char*)&g_bus.bgp, 1); f.write((char*)&g_bus.obp0, 1); f.write((char*)&g_bus.obp1, 1);
    f.write((char*)&g_bus.ly, 1);
    f.write((char*)&g_bus.vbk, 1); f.write((char*)&g_bus.bcps, 1); f.write((char*)&g_bus.ocps, 1);
    f.write((char*)g_bus.bgPaletteRam.data(), 64);
    f.write((char*)g_bus.objPaletteRam.data(), 64);
    int32_t pmc = g_bus.ppuModeCycles; f.write((char*)&pmc, 4);
    int32_t wil = g_bus.windowInternalLine; f.write((char*)&wil, 4);
    f.write((char*)g_bus.vram.data(), 0x2000);
    f.write((char*)g_bus.vram1.data(), 0x2000);
    f.write((char*)g_bus.ext_ram.data(), 0x20000);
    f.write((char*)g_bus.wram.data(), 0x2000);
    f.write((char*)g_bus.oam.data(), 0xA0);
    f.write((char*)g_bus.io.data(), 0x80);
    f.write((char*)g_bus.hram.data(), 0x80);
    f.write((char*)&g_bus.ie, 1);
    f.write((char*)&g_bus.romBank, 2);
    uint8_t re = g_bus.ramEnabled; f.write((char*)&re, 1);
    f.write((char*)&g_bus.ramBank, 1);
    f.write((char*)&g_bus.mbcMode, 1);
    f.write((char*)&g_bus.mbcType, 1);
    uint8_t dma = g_bus.dmaActive; f.write((char*)&dma, 1);
    int32_t dr = g_bus.dmaCyclesRemaining; f.write((char*)&dr, 4);
    f.write((char*)&g_bus.joypSelect, 1);
    f.write((char*)&g_bus.buttonsState, 1);
    // RTC
    uint8_t rtcs = g_bus.rtcSelected ? 1 : 0;
    f.write((char*)&rtcs, 1);
    f.write((char*)&g_bus.rtcReg, 1);
    f.write((char*)g_bus.rtcRegs, 5);
    return f.good();
}

bool loadStateFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    // Sanity: check magic/version before touching anything
    {
        std::ifstream g(path, std::ios::binary);
        if (!g) return false;
        uint32_t magic=0, ver=0;
        g.read((char*)&magic, 4); g.read((char*)&ver, 4);
        if (magic != Bus::kStateMagic || ver != Bus::kStateVersion) return false;
    }
    f.seekg(0);
    // Read into a temp copy
    Bus tmp = g_bus;
    uint32_t magic=0,ver=0;
    f.read((char*)&magic,4); f.read((char*)&ver,4);
    f.read((char*)&tmp.pc, 2); f.read((char*)&tmp.sp, 2);
    f.read((char*)&tmp.a, 1); f.read((char*)&tmp.f, 1);
    f.read((char*)&tmp.b, 1); f.read((char*)&tmp.c, 1);
    f.read((char*)&tmp.d, 1); f.read((char*)&tmp.e, 1);
    f.read((char*)&tmp.h, 1); f.read((char*)&tmp.l, 1);
    uint8_t bi,bie,bh,bhp;
    f.read((char*)&bi,1); f.read((char*)&bie,1); f.read((char*)&bh,1); f.read((char*)&bhp,1);
    tmp.ime=bi!=0; tmp.imeEnablePending=bie!=0; tmp.halted=bh!=0; tmp.haltBugPending=bhp!=0;
    f.read((char*)&tmp.divCounter,2);
    f.read((char*)&tmp.tima,1); f.read((char*)&tmp.tma,1); f.read((char*)&tmp.tac,1);
    uint8_t btop; f.read((char*)&btop,1); tmp.timaOverflowPending = btop!=0;
    int32_t tdel; f.read((char*)&tdel,4); tmp.timaOverflowDelay = tdel;
    f.read((char*)&tmp.lcdc,1); f.read((char*)&tmp.stat,1);
    f.read((char*)&tmp.scy,1); f.read((char*)&tmp.scx,1);
    f.read((char*)&tmp.lyc,1); f.read((char*)&tmp.wy,1); f.read((char*)&tmp.wx,1);
    f.read((char*)&tmp.bgp,1); f.read((char*)&tmp.obp0,1); f.read((char*)&tmp.obp1,1);
    f.read((char*)&tmp.ly,1);
    f.read((char*)&tmp.vbk,1); f.read((char*)&tmp.bcps,1); f.read((char*)&tmp.ocps,1);
    f.read((char*)tmp.bgPaletteRam.data(),64);
    f.read((char*)tmp.objPaletteRam.data(),64);
    int32_t pmc; f.read((char*)&pmc,4); tmp.ppuModeCycles = pmc;
    int32_t wil; f.read((char*)&wil,4); tmp.windowInternalLine = wil;
    f.read((char*)tmp.vram.data(),0x2000);
    f.read((char*)tmp.vram1.data(),0x2000);
    f.read((char*)tmp.ext_ram.data(),0x20000);
    f.read((char*)tmp.wram.data(),0x2000);
    f.read((char*)tmp.oam.data(),0xA0);
    f.read((char*)tmp.io.data(),0x80);
    f.read((char*)tmp.hram.data(),0x80);
    f.read((char*)&tmp.ie,1);
    f.read((char*)&tmp.romBank,2);
    uint8_t re; f.read((char*)&re,1); tmp.ramEnabled = re!=0;
    f.read((char*)&tmp.ramBank,1);
    f.read((char*)&tmp.mbcMode,1);
    f.read((char*)&tmp.mbcType,1);
    uint8_t dma; f.read((char*)&dma,1); tmp.dmaActive = dma!=0;
    int32_t dr; f.read((char*)&dr,4); tmp.dmaCyclesRemaining = dr;
    f.read((char*)&tmp.joypSelect,1);
    f.read((char*)&tmp.buttonsState,1);
    uint8_t rtcs; f.read((char*)&rtcs,1); tmp.rtcSelected = rtcs!=0;
    f.read((char*)&tmp.rtcReg,1);
    f.read((char*)tmp.rtcRegs,5);
    if (!f.good()) return false;
    if (tmp.ly > 153) tmp.ly = 0;
    if (tmp.romBank == 0) tmp.romBank = 1;
    g_bus = tmp;
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
        "Rew. Reborn v0.6.2-alpha\n"
        "Emulador Game Boy / Game Boy Color\n\n"
        "Novedades v0.6.2:\n"
        "- Tomb Raider: corregidos glitches gr\341ficos y de audio\n"
        "- Pokemon Crystal: corregida la banca WRAM\n"
        "- Nuevas funciones Extra\n"
        "- Correcci\xF3n de varios bugs\n\n"
        "Estado actual:\n"
        "- CPU, PPU, APU funcionales\n"
        "- MBC1 / MBC3 / MBC5\n"
        "- Save SRAM + Save States\n"
        "- Doble velocidad CGB (parcial)\n"
        "- HDMA con timing HBlank\n"
        "- Paletas CGB y KEY1\n"
        "- RTC en MBC3 (parcial)\n"
        "- Oversampling 8x en audio\n"
        "- Joypad 4 columnas configurables\n"
        "- Frame limiter a 59.7 FPS\n\n"
        "En desarrollo:\n"
        "- Precisi\xF3n APU fina (dmg_sound)\n"
        "- Detecci\xF3n SGB",
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
            for (int a = 0; a < 8; ++a)
                for (int c = 0; c < 4; ++c)
                    if (g_bindings[a][c] == vk && vk != 0) {
                        g_bus.setButton(a, true);
                    }
            break;
        }
        case WM_KEYUP: {
            uint32_t vk = (uint32_t)wp;
            for (int a = 0; a < 8; ++a)
                for (int c = 0; c < 4; ++c)
                    if (g_bindings[a][c] == vk && vk != 0) {
                        g_bus.setButton(a, false);
                    }
            break;
        }
        case WM_KILLFOCUS:
            g_bus.releaseAllButtons();
            g_auto_fire_enabled[0] = g_auto_fire_enabled[1] = false;
            break;

        case WM_COMMAND:
            if (EsComandoMenu(hWnd, LOWORD(wp), "Exit")) { DestroyWindow(hWnd); return 0; }
            if (EsComandoMenu(hWnd, LOWORD(wp), "About") || EsComandoMenu(hWnd, LOWORD(wp), "About...")) { MostrarAcercaDe(hWnd); return 0; }
            if (LOWORD(wp) == ID_FILE_LOADROM) OpenRomDialog(hWnd);
            if (LOWORD(wp) == ID_FILE_FREEROM) LiberarRomActual(hWnd);
            if (LOWORD(wp) == ID_FILE_RESET)   ResetearEmulador(hWnd);
            if (LOWORD(wp) == ID_FILE_PAUSE)   AlternarPausaEmulador(hWnd);
            if (LOWORD(wp) == ID_OPTION_KEY) DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_KEY_SETTING), hWnd, KeySettingProc, 0);
            if (LOWORD(wp) == ID_OPTION_AUDIO_ENABLE) {
                g_audio_enabled = !g_audio_enabled;
                CheckMenuItem(hM, ID_OPTION_AUDIO_ENABLE, g_audio_enabled ? MF_CHECKED : MF_UNCHECKED);
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
            bmi.bmiHeader.biWidth = GB_WIDTH;
            bmi.bmiHeader.biHeight = -GB_HEIGHT;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(hdc, 0, 0, cr.right, cr.bottom,
                          0, 0, GB_WIDTH, GB_HEIGHT,
                          g_screenBuffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
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

    RECT rc = {0, 0, GB_WIDTH * g_window_scale, GB_HEIGHT * g_window_scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);
    HWND hWnd = CreateWindowExA(0, "RewRebornClass", "Rew. Reborn v0.6.2-alpha", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);
    ShowWindow(hWnd, nShow); UpdateWindow(hWnd);

    // [v0.4] Init audio
    IniciarAudio(hWnd);
    CheckMenuRadioItem(GetMenu(hWnd), ID_VIDEO_X1, ID_VIDEO_X4,
                       ID_VIDEO_X1 + (g_window_scale - 1), MF_BYCOMMAND);
    // Audio menu check
    if (GetMenu(hWnd)) {
        CheckMenuItem(GetMenu(hWnd), ID_OPTION_AUDIO_ENABLE, g_audio_enabled ? MF_CHECKED : MF_UNCHECKED);
    }

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
                int cyclesThisFrame = 0;
                while (cyclesThisFrame < 70224) {
                    int c = g_bus.step();
                    if (c == 0) c = 4;
                    cyclesThisFrame += c;
                }
				// NUEVO: procesar el APU una sola vez por frame completo
                g_bus.apu.tick(70224);
                // [v0.4] Auto Fire y audio
                ActualizarAutoFire();
                EnviarFrameAudio();				
                InvalidateRect(hWnd, NULL, FALSE);
                UpdateWindow(hWnd);

                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                LONGLONG elapsed = now.QuadPart - lastFrameTime.QuadPart;
                if (elapsed < ticksPerFrame) {
                    LONGLONG remaining = ticksPerFrame - elapsed;
                    DWORD sleepMs = (DWORD)(remaining / (qpcFreq.QuadPart / 1000));
                    if (sleepMs > 3) Sleep(sleepMs - 2);
                    do { QueryPerformanceCounter(&now); }
                    while ((now.QuadPart - lastFrameTime.QuadPart) < ticksPerFrame);
                }
                // ★ Contador de FPS (actualiza cada 1 segundo)
                fpsFrameCount++;
                LARGE_INTEGER fpsNow;
                QueryPerformanceCounter(&fpsNow);
                double elapsedSec = (double)(fpsNow.QuadPart - fpsTimerStart.QuadPart) / qpcFreq.QuadPart;
                if (elapsedSec >= 1.0) {
                    double fps = fpsFrameCount / elapsedSec;
                    int fps10 = (int)(fps * 10);  // un decimal
                    char dbg[64];
                    wsprintfA(dbg, "Rew. Reborn v0.6.2-alpha - %d.%d FPS", fps10 / 10, fps10 % 10);
                    SetWindowTextA(hWnd, dbg);
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