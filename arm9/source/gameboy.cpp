#include <nds.h>
#include <stdlib.h>
#include <stdio.h>
#include "gameboy.h"
#include "gbcpu.h"
#include "gbgfx.h"
#include "gbsnd.h"
#include "mmu.h"
#include "timer.h"
#include "main.h"
#include "inputhelper.h"
#include "nifi.h"
#include "console.h"
#include "cheats.h"
#include "sgb.h"

int mode2Cycles, mode3Cycles;
int scanlineCounter;
int doubleSpeed;

time_t rawTime;
time_t lastRawTime;

int fps;
bool fpsOutput=true;
bool timeOutput=true;
bool fastForwardMode = false; // controlled by the menu
bool fastForwardKey = false;  // only while its hotkey is pressed

bool yellowHack;

// ...what is phase? I think I made that up. Used for timing when the gameboy 
// screen is off.
int phaseCounter;
int dividerCounter;
int serialCounter;

int timerCounter;
int timerPeriod;
int serialPeriod;
int timerPeriods[4]  = { clockSpeed/4096, clockSpeed/262144, clockSpeed/65536, clockSpeed/16384 };
int serialPeriods[2] = { clockSpeed/8192, clockSpeed/262144 };

int gbMode;
bool sgbMode;

int cyclesToEvent;
int maxWaitCycles;

bool resettingGameboy = false;

bool probingForBorder=false;


inline void setEventCycles(int cycles) {
    if (!cyclesToEvent || cycles < cyclesToEvent) {
        cyclesToEvent = cycles;
    }
}

// Called once every gameboy vblank
void updateInput() {
    if (probingForBorder)
        return;

    if (cheatsEnabled)
        applyGSCheats();

    readKeys();
    handleEvents();		// Input mostly
    if (!consoleDebugOutput && (rawTime > lastRawTime))
    {
        consoleClear();
        int line=0;
        if (fpsOutput) {
            consoleClear();
            iprintf("FPS: %d\n", fps);
            line++;
        }
        fps = 0;
        if (timeOutput) {
            for (; line<23-1; line++)
                iprintf("\n");
            char *timeString = ctime(&rawTime);
            for (int i=0;; i++) {
                if (timeString[i] == ':') {
                    timeString += i-2;
                    break;
                }
            }
            char s[50];
            strncpy(s, timeString, 50);
            s[5] = '\0';
            int spaces = 31-strlen(s);
            for (int i=0; i<spaces; i++)
                iprintf(" ");
            iprintf("%s\n", s);
        }
        lastRawTime = rawTime;
    }
}

void resetGameboy() {
    cyclesToExecute = 0;
    resettingGameboy = true;
}

int soundCycles=0;
int extraCycles;
void runEmul()
{
    int cycles;

    for (;;)
    {
emuLoopStart:
        cyclesToEvent -= extraCycles;
        extraCycles = 0;
        if (halt)
            cycles = cyclesToEvent;
        else
            cycles = runOpcode(cyclesToEvent);

        cyclesToEvent = 0;

        if (ioRam[0x02]&0x81) {
            serialCounter += cycles;
            if (serialCounter >= serialPeriod) {
                serialCounter -= serialPeriod;
                packetData = 0xff;
                transferReady = true;
            }

            setEventCycles(serialPeriod-serialCounter);

            if (transferReady) {
                if (!(ioRam[0x02] & 1)) {
                    sendPacketByte(56, sendData);
                }
                timerStop(2);
                ioRam[0x01] = packetData;
                requestInterrupt(SERIAL);
                ioRam[0x02] &= ~0x80;
                packetData = -1;
                transferReady = false;
            }
        }

        updateTimers(cycles);
        soundCycles += cycles;
        if (soundCycles >= 6666) {
            updateSound(soundCycles);
            soundCycles = 0;
        }

        updateLCD(cycles);
        
        if (resettingGameboy) {
            initializeGameboy();
            resettingGameboy = false;
            goto emuLoopStart;
        }

        int interruptTriggered = ioRam[0x0F] & ioRam[0xFF];
        if (interruptTriggered)
            extraCycles += handleInterrupts(interruptTriggered);
    }
}

void initLCD()
{
    // Pokemon Yellow hack: I need to intentionally SLOW DOWN emulation for 
    // Pikachu's pitch to sound right...
    yellowHack = strcmp(getRomTitle(), "POKEMON YELLOW") == 0;
    if (yellowHack && !(fastForwardMode || fastForwardKey))
        maxWaitCycles = 50;
    else
        maxWaitCycles = 800;

    setDoubleSpeed(0);

    ioRam[0x41] = 2|(ioRam[0x41]&~3);

    scanlineCounter = 0;
    phaseCounter = 0;
    timerCounter = 0;
    dividerCounter = 0;

    serialPeriod = serialPeriods[0];
    timerPeriod = timerPeriods[0];

    timerStop(2);
}

// Called either from startup, or when the BIOS writes to FF50.
void initGameboyMode() {
    gbRegs.af.b.l = 0xB0;
    gbRegs.bc.w = 0x0013;
    gbRegs.de.w = 0x00D8;
    gbRegs.hl.w = 0x014D;
    switch(resultantGBMode) {
        case 0: // GB
            gbRegs.af.b.h = 0x01;
            gbMode = GB;
            if (rom[0][0x143] == 0x80 || rom[0][0x143] == 0xC0)
                // Init the palette in case the bios overwrote it, since it 
                // assumed it was starting in GBC mode.
                initGFXPalette();
            break;
        case 1: // GBC
            gbRegs.af.b.h = 0x11;
            if (gbaModeOption)
                gbRegs.bc.b.h |= 1;
            gbMode = CGB;
            break;
        case 2: // SGB
            sgbMode = true;
            gbRegs.af.b.h = 0x01;
            gbMode = GB;
            initSGB();
            break;
    }
}

void checkLycCoincidence (void) 
{
    if (ioRam[0x44] == ioRam[0x45]) {
        ioRam[0x41] |= 4;
        if (ioRam[0x41]&(1<<6))
            requestInterrupt(LCD);
    } 
    else {
        ioRam[0x41] &= ~4;
    }
}

inline void updateLCD(int cycles)
{
    #define setVideoMode(x) (ioRam[0x41] = (ioRam[0x41]&~3)|x)

    scanlineCounter += cycles;

    if (!(ioRam[0x40] & 0x80))		// If LCD is off
    {
        if (scanlineCounter >= 456*153) {
            scanlineCounter -= 456*153;
            ioRam[0x44] = 0;
            ioRam[0x40] |= 0x80;
            setVideoMode(0);

            if (ioRam[0x41]&(1<<5))
                requestInterrupt(LCD);

            checkLycCoincidence();

            fps++;
            drawScreen();
            updateInput();

            return;
        }
        else
            setEventCycles(456*153-scanlineCounter);
    }

    switch(ioRam[0x41]&3)
    {
        case 2:
            {
                if (scanlineCounter >= 80) {
                    scanlineCounter -= 80;
                    setVideoMode(3);
                    setEventCycles(172-scanlineCounter);
                }
                else
                    setEventCycles(80-scanlineCounter);
            }
            break;
        case 3:
            {
                if (scanlineCounter >= 172) {
                    scanlineCounter -= 172;
                    setVideoMode(0);

                    /* HBLANK int */
                    if (ioRam[0x41]&(1<<3))
                        requestInterrupt(LCD);

                    drawScanline(ioRam[0x44]);
                    drawScanlinePalettes(ioRam[0x44]);

                    setEventCycles(204-scanlineCounter);
                }
                else
                    setEventCycles(172-scanlineCounter);
            }
            break;
        case 0:
            {
                if (scanlineCounter >= 204) {
                    scanlineCounter -= 204;
                    ioRam[0x44]++;

                    checkLycCoincidence();

                    if (!halt && updateHblankDMA())
                        extraCycles += 50;

                    if (ioRam[0x44] < 144) {
                        // This breaks instr_timing
                        setVideoMode(2);
                        setEventCycles(80-scanlineCounter);
                        /* Mode2 OAM interrupt */
                        if (ioRam[0x41]&(1<<5))
                            requestInterrupt(LCD);
                        return;
                    }
                    /* Enter the vblank */
                    else {
                        setVideoMode(1);
                        setEventCycles(456-scanlineCounter);
                        return;
                    }
                }
                else
                    setEventCycles(204-scanlineCounter);
            }
            break;
        case 1:
            {
                /* VBlank */
                if (scanlineCounter >= 456) {
                    scanlineCounter -= 456;

                    if (ioRam[0x44] == 144) {
                        requestInterrupt(VBLANK);
                        /* VBlank int */
                        if (ioRam[0x41]&(1<<4))
                            requestInterrupt(LCD);

                        fps++;
                        drawScreen();
                        updateInput();
                    }

                    ioRam[0x44]++;
                    checkLycCoincidence();

                    if (ioRam[0x44] == 153) {
                        setVideoMode(2);
                        setEventCycles(80-scanlineCounter);
                        ioRam[0x44] = 0;
                        if (ioRam[0x41]&(1<<5))
                            requestInterrupt(LCD);
                        return;
                    }

                    setEventCycles(456-scanlineCounter);
                }
                else
                    setEventCycles(456-scanlineCounter);
            }
            break;
    }
}
// TODO : DOuble speed
inline void updateTimers(int cycles)
{
    if (ioRam[0x07] & 0x4)
    {
        timerCounter += cycles;
        while (timerCounter >= timerPeriod)
        {
            timerCounter -= timerPeriod;
            ioRam[0x05]++;
            if (ioRam[0x05] == 0xff)
            {
                requestInterrupt(TIMER);
                ioRam[0x05] = ioRam[0x06];
            }
        }
        setEventCycles(timerPeriod-timerCounter);
    }
    dividerCounter += cycles;
    while (dividerCounter >= 256)
    {
        dividerCounter -= 256;
        ioRam[0x04]++;
    }
    //setEventCycles(dividerCounter);
}


void requestInterrupt(int id)
{
    ioRam[0x0F] |= id;
    if (ioRam[0x0F] & ioRam[0xFF])
        cyclesToExecute = 0;
}

void setDoubleSpeed(int val) {
    if (val == 0) {
        mode2Cycles = 456 - 80;
        mode3Cycles = 456 - 172 - 80;
        doubleSpeed = 0;
        ioRam[0x4D] &= ~0x80;
    }
    else {
        mode2Cycles = (456 - 80)*2;
        mode3Cycles = (456 - 172 - 80)*2;
        doubleSpeed = 1;
        ioRam[0x4D] |= 0x80;
    }
}
