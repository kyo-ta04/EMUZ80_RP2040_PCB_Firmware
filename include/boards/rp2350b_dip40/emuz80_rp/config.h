// Z80 bus emulator for RP2350B-DIP40
//
// Copyright (C) 2026 DragonBallEZ (kyo-ta04)
// https://github.com/kyo-ta04/EMUZ80_RP2040_PCB_Firmware
//
// SPDX-License-Identifier: MIT
// See LICENSE file for details.
//

// GPIO Pin Definitions
#define ADRS_LOW_BASE 0  // Address Bus A0-7
#define ADRS_HIGH_BASE 22  // Address Bus A8-15
#define DATA_BASE 14  // Data Bus D0 - D7
#define IORQ_PIN 8
#define MREQ_PIN 9
#define RD_PIN 13
#define WR_PIN 36
#define WAIT_PIN 12
#define RESET_PIN 37
#define CLK_PIN 11
#define RFSH_PIN 10
#define INT_PIN 38
#define VCC_5V_EN_PIN 35

#define MEMORY_SIZE 65536 // 64KB
#define UART_TX_BUF_SIZE 256

//#define CONFIG_ROM_BASIC
#define CONFIG_ROM_CPM

#include "AE-RP2040.pio.h"
