// Z80 bus emulator for Waveshare RP2350-Zero
//
// Copyright (C) 2026 DragonBallEZ (kyo-ta04)
// https://github.com/kyo-ta04/EMUZ80_RP2040_PCB_Firmware
//
// SPDX-License-Identifier: MIT
// See LICENSE file for details.
//

// GPIO Pin Definitions
#define ADRS_BASE 0  // GP0..15: Address Bus A0-15
#define DATA_BASE 16 // GP16..23: Data Bus
// #define IORQ_PIN 24  // GP24: IORQ
#define MREQ_PIN 24 // GP24: MREQ
#define RD_PIN 25   // GP25: RD
#define WR_PIN 26   // GP26: WR
// #define WAIT_PIN 27  // GP27: WAIT
#define PA0_PIN 27   // GP27: PPI PA b0
#define RESET_PIN 28 // GP28: RESET
#define CLK_PIN 29   // GP29: CLK

#define MEMORY_SIZE 65536 // 64KB
#define UART_TX_BUF_SIZE 256

//#define CONFIG_ROM_BASIC
#define CONFIG_ROM_CPM

#include "AE-RP2040.pio.h"
