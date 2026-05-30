// EMUZ80_RP2040_PCB_Firmware.c - Z80 bus emulator for Akizuki AE-RP2040
//
// Copyright (C) 2026 DragonBallEZ (kyo-ta04)
// https://github.com/kyo-ta04/EMUZ80_RP2040_PCB_Firmware
//
// SPDX-License-Identifier: MIT
// See LICENSE file for details.

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#endif
#include "hardware/dma.h"         // これをファイル上部に追加
#include "hardware/structs/sio.h" // ← SIO直叩きに使用（最小限）
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h" // TinyUSBのヘッダーを追加
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AE-RP2040.pio.h" // PIO program headers (auto-generated)

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

// Z80用メモリー
#define MEMORY_SIZE 65536 // 64KB
static uint8_t __attribute__((aligned(4))) memory[MEMORY_SIZE] = {
    [0 ... MEMORY_SIZE - 1] = 0xFF};
volatile bool stop_flg = false;

// UART/USB 共有バッファ
#define UART_TX_BUF_SIZE 256
#if 0
volatile uint8_t __attribute__((section(".scratch_x.uart"))) uart_tx_buf[UART_TX_BUF_SIZE];
volatile uint16_t __attribute__((section(".scratch_x.uart"))) uart_tx_head = 0; // コア1 (Z80側) が更新
volatile uint16_t __attribute__((section(".scratch_x.uart"))) uart_tx_tail = 0; // コア0 (UART側) が更新

volatile uint8_t __attribute__((section(".scratch_x.uart"))) uart_rxdata = 0;
volatile uint8_t __attribute__((section(".scratch_x.uart"))) uart_stat = 0;
#else
volatile uint8_t
    __attribute__((section(".scratch_y.uart"))) uart_tx_buf[UART_TX_BUF_SIZE];
volatile uint16_t __attribute__((section(".scratch_y.uart"))) uart_tx_head =
    0; // コア1 (Z80側) が更新
volatile uint16_t __attribute__((section(".scratch_y.uart"))) uart_tx_tail =
    0; // コア0 (UART側) が更新

volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_rxdata = 0;
volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_stat = 0;
#endif

#define UART_RX_READY 0xFF

// BOOT ROM
const unsigned char boot[] = {
    0xC3,
    0x00,
    0xFA, // JP BIOS
};
const size_t boot_size = sizeof(boot);

// ====================== ROM/BIOSデータ (extern宣言) ======================
// 各データは個別の .c ファイルでコンパイルされる
#include "bios01.h"   // BIOSコード
#include "ccp_bdos.h" // CCP BDOSコード
#include "cpm22_1.h"  // CPM 2.2 Disk Image (Drive A: IBM 8" SD)
#if 0
#include "cpm22_disk1.h"    // CPM 2.2 Disk Image (Drive B: IBM 8" HD)
#include "cpm22_tp301a.h"   // CPM 2.2 Disk Image (Drive C: IBM 8" SD)
#include "cpm22_z80forth.h" // CPM 2.2 Disk Image (Drive D: IBM 8" SD)
#include "cpm22_htc.h"      // CPM 2.2 Disk Image (Drive I: 650KB Custom)
#endif

// ====================== 仮想ディスク定義 ======================
// cpm2c.pyで生成された各ROM配列を一つのテーブルにまとめる
#define ROMDISK_SIZE (256 * 1024) // (128*26*77=256,256 / 256*1024=262,144)
const uint8_t *const rom_disks[] = {cpm22_1, cpm22_1, cpm22_1, cpm22_1}; /*
// デバッグ時は1ドライブ */
// const uint8_t *const rom_disks[] = {cpm22_1, cpm22_disk1, tp301a, z80forth};

// B: 仮想RAMディスク (Read/Write) - 十分なサイズを確保
#define RAMDISK_SIZE (128 * 1024) // 128KB 262,144 (128*26*39)=256256
static uint8_t __attribute__((aligned(4))) ramdisk[RAMDISK_SIZE] = {
    [0 ... RAMDISK_SIZE - 1] = 0xE5 // E5で埋めて未使用にする
};

//
// --- Helper: Manual Clock Pulse ---
static void clk_on_off(int n) {
  gpio_set_function(CLK_PIN, GPIO_FUNC_SIO);
  gpio_set_dir(CLK_PIN, GPIO_OUT);
  for (int i = 0; i < n; i++) {
    gpio_put(CLK_PIN, 1);
    sleep_ms(10); // 短いパルス
    gpio_put(CLK_PIN, 0);
    sleep_ms(10);
  }
}

// --- Helper: Delayed RESET OFF ---
static int64_t reset_off_callback(alarm_id_t id, void *user_data) {
  printf("RESET-OFF (Delayed)\n\n");
  gpio_put(RESET_PIN, 1); // RESET-OFF (High)
  return 0;               // ONE_SHOT
}

// ====================== Z80 CLK PWM 制御 ======================
static uint clk_pwm_slice_num;
static uint clk_pwm_channel;
static bool clk_pwm_initialized = false;
static uint32_t current_clk_freq = 0;

static void clk_pwm_init(void) {
  clk_pwm_slice_num = pwm_gpio_to_slice_num(CLK_PIN);
  clk_pwm_channel = pwm_gpio_to_channel(CLK_PIN);
  gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_clkdiv(&cfg, 1.0f);
  pwm_config_set_wrap(&cfg, 0);
  pwm_init(clk_pwm_slice_num, &cfg, false);
  pwm_set_chan_level(clk_pwm_slice_num, clk_pwm_channel, 0);

  clk_pwm_initialized = true;
}

static void clk_pwm_set_frequency(uint32_t freq_hz) {
  if (!clk_pwm_initialized) {
    clk_pwm_init();
  }

  uint32_t sys_clk = clock_get_hz(clk_sys);

  float clkdiv = 1.0f;
  uint32_t wrap = (sys_clk / freq_hz) - 1;

  if (wrap > 65535) {
    clkdiv = (float)sys_clk / (freq_hz * 65536LL);
    if (clkdiv > 255.9375f) {
      clkdiv = 255.9375f;
    }
    wrap = (uint32_t)((float)sys_clk / (freq_hz * clkdiv)) - 1;
    if (wrap > 65535) {
      wrap = 65535;
    }
  }

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_clkdiv(&cfg, clkdiv);
  pwm_config_set_wrap(&cfg, (uint16_t)wrap);
  pwm_init(clk_pwm_slice_num, &cfg, false);
  pwm_set_chan_level(clk_pwm_slice_num, clk_pwm_channel, (wrap + 1) / 2);

  current_clk_freq = freq_hz;
}

static inline void clk_pwm_output_on(void) {
  pwm_set_enabled(clk_pwm_slice_num, true);
}

static inline void clk_pwm_output_off(void) {
  pwm_set_enabled(clk_pwm_slice_num, false);
}

static void init_clk_pwm(uint32_t freq_hz) {
  clk_pwm_init();
  clk_pwm_set_frequency(freq_hz);
  clk_pwm_output_on();
}

// PIO0
const uint sm_trg_wr = 0;
const uint sm_trg_rd = 1;
const uint sm_emu = 2;
#define pio_emu pio0
// PIO1
const uint sm_dirsL = 0;
const uint sm_dirsH = 1;
const uint sm_data_out = 2;
#define pio_data_out pio1

// --- PIO Helpers ---
void pio_init_bus() {
  PIO pio;

  // PIO 0
  pio = pio0;

  // SM: trg_wr (Detection of falling edge on WR)
  uint offset_trg_wr = pio_add_program(pio, &trg_rw_program);
  pio_sm_config c_trg_wr = trg_rw_program_get_default_config(offset_trg_wr);
  sm_config_set_in_pins(&c_trg_wr, WR_PIN);

  pio_sm_init(pio, sm_trg_wr, offset_trg_wr, &c_trg_wr);
  pio_sm_set_enabled(pio, sm_trg_wr, true);

  // SM: trg_rd (Detection of falling edge on RD)
  uint offset_trg_rd = pio_add_program(pio, &trg_rw_program);
  pio_sm_config c_trg_rd = trg_rw_program_get_default_config(offset_trg_rd);
  sm_config_set_in_pins(&c_trg_rd, RD_PIN);

  pio_sm_init(pio, sm_trg_rd, offset_trg_rd, &c_trg_rd);
  pio_sm_set_enabled(pio, sm_trg_rd, true);

  // SM: m_emu (Address/Data handling)
  uint offset_emu = pio_add_program(pio, &m_emu_program);
  pio_sm_config c_emu = m_emu_program_get_default_config(offset_emu);
  sm_config_set_in_pins(&c_emu, 0);
  sm_config_set_in_shift(&c_emu, false, true,
                         30); // shift left, auto_push=true, threshold=30
  pio_sm_init(pio, sm_emu, offset_emu, &c_emu);
  pio_sm_set_enabled(pio, sm_emu, true);

  // PIO 1
  pio = pio1;
  for (int i = 0; i < 8; i++) {
    pio_gpio_init(pio, DATA_BASE + i);
  }

  // SM: dirsL
  uint offset_dirs = pio_add_program(pio, &d_pindirs_program);
  pio_sm_config c_dirsL = d_pindirs_program_get_default_config(offset_dirs);
  sm_config_set_set_pins(&c_dirsL, DATA_BASE, 4); // D0..D3
  sm_config_set_in_pins(&c_dirsL, RD_PIN);
  pio_sm_init(pio, sm_dirsL, offset_dirs, &c_dirsL);
  pio_sm_set_enabled(pio, sm_dirsL, true);

  // SM: dirsH
  pio_sm_config c_dirsH = d_pindirs_program_get_default_config(offset_dirs);
  sm_config_set_set_pins(&c_dirsH, DATA_BASE + 4, 4); // D4..D7
  sm_config_set_in_pins(&c_dirsH, RD_PIN);
  pio_sm_init(pio, sm_dirsH, offset_dirs, &c_dirsH);
  pio_sm_set_enabled(pio, sm_dirsH, true);

  // SM: data_out (Output to data bus)
  uint offset_data_out = pio_add_program(pio, &data_out_program);
  pio_sm_config c_data_out =
      data_out_program_get_default_config(offset_data_out);
  sm_config_set_out_pins(&c_data_out, DATA_BASE, 8);
  sm_config_set_out_shift(&c_data_out, true, true,
                          8); // shift right, auto_pull=true, threshold=8
  pio_sm_init(pio, sm_data_out, offset_data_out, &c_data_out);
  pio_sm_set_enabled(pio, sm_data_out, true);
}

// --- UART Task (Core 0) ---
void task1(void) {
  printf("task1 UART start..\n");
  while (true) {
    // 送信処理 (Z80 -> USB)
    while (uart_tx_head != uart_tx_tail) { // バッファが空でない間
      if (tud_cdc_connected() && tud_cdc_write_available() > 0) {
        uint8_t ch = uart_tx_buf[uart_tx_tail];
        putchar(ch);
        uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
      } else {
        break; // 今は送信できないので次回に持ち越し
      }
    }
    // 受信処理(US->Z80) RX Readyが0(空)の場合のみ入力をチェック
    if (!(uart_stat)) {
      int c = getchar_timeout_us(0);
      if (c != PICO_ERROR_TIMEOUT) {
        uart_rxdata = (uint8_t)c;
        uart_stat = 0xFF; // RX Data Available
      }
    }
    sleep_us(500);
  }
}

// グローバル領域（emu_loopの外側）
static int disk_dma_chan = -1;         // DMAチャネル番号（-1 = 未初期化）
static volatile bool dma_busy = false; // DMA転送中フラグ

void init_disk_dma(void) {
  if (disk_dma_chan < 0) {
    disk_dma_chan = dma_claim_unused_channel(true); // 自動で空きチャネル取得
    printf("Disk DMA channel allocated: %d\n",
           disk_dma_chan); // デバッグ用（後で削除可）
  }
}

// I/O用の状態変数（レジスタ枯渇を防ぐためグローバル配置）
static uint8_t current_drive = 0;
static uint8_t current_track = 0;
static uint8_t current_sector = 0;
static uint8_t read_write = 0;
static uint8_t fdc_status = 0;
static uint8_t dma_addr_low = 0;
static uint8_t dma_addr_high = 0;

void __attribute__((noinline)) handle_io_write(uint32_t ioadrs,
                                               uint8_t data_byte) {
  switch (ioadrs) {
  case 0x01: { // CONOUT : ポート1
    uint8_t next = (uart_tx_head + 1) % UART_TX_BUF_SIZE;
    if (next != uart_tx_tail) {
      uart_tx_buf[uart_tx_head] = data_byte;
      uart_tx_head = next;
    }
    // 満杯時は文字を捨てる
    break;
  }
  case 0x0A: // 10:0x0A : ドライブ選択
    current_drive = data_byte;
    break;
  case 0x0B: // 11:0x0B : トラック選択
    current_track = data_byte;
    break;
  case 0x0C: // 12:0x0C : セクタ選択
    current_sector = data_byte;
    break;
  case 0x0D: { // 13:0x0D:FDCOPコマンド(0=Read,1=Write)
    read_write = data_byte;
    uint16_t dma_addr_z80 = ((uint16_t)dma_addr_high << 8) | dma_addr_low;
    // オフセット計算 (128バイト * (トラック * 26 + セクタ-1))
    uint32_t logical_sector = (current_sector >= 1) ? (current_sector - 1) : 0;
    uint32_t disk_offset =
        ((uint32_t)current_track * 26 + logical_sector) * 128UL;

    if (read_write == 0) { // ============== READ ==============
      const uint8_t *src = NULL;
      uint32_t max_size = 0;

      if (current_drive <= 3) { // A, B, C, D (ROM 256KB)
        src = rom_disks[current_drive];
        max_size = ROMDISK_SIZE;
      } else if (current_drive == 8) { // I: (ROM 650KB)
//        src = cpm22_htc; /* htc.c で定義、デバッグ時はコメントアウト */
//        max_size = ROM_DISK_I_SIZE;
      } else if (current_drive == 9) { // J: (RAM 128KB)
        src = ramdisk;
        max_size = RAMDISK_SIZE;
      }
      if (src && (disk_offset + 128 <= max_size)) {
        if (dma_channel_is_busy(disk_dma_chan)) {
          dma_channel_abort(disk_dma_chan);
        }
        dma_channel_config c = dma_channel_get_default_config(disk_dma_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8); // 8bit
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, true);
        dma_channel_configure(disk_dma_chan, &c,
                              &memory[dma_addr_z80], // 書き込み先（Z80メモリ）
                              src + disk_offset,     // 読み出し元
                              128,                   // 転送数（バイト）
                              true);                 // 即開始
        dma_busy = true;                             // DMA開始
        fdc_status = 0; // 即OK返却（DMAはバックグラウンド）
      } else {
        memset(&memory[dma_addr_z80], 0xE5, 128);
        fdc_status = 1;
      }

    } else { // ================== WRITE ==================
      uint8_t *dst = NULL;
      uint32_t max_size = 0;
      if (!(current_drive == 9)) { // J : RAM のみ書き込み許可
        fdc_status = 1;
      } else {
        dst = ramdisk;
        max_size = RAMDISK_SIZE;
        if (dst && disk_offset + 128 <= max_size && disk_dma_chan >= 0) {
          dma_channel_config c = dma_channel_get_default_config(disk_dma_chan);
          channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
          channel_config_set_read_increment(&c, true);
          channel_config_set_write_increment(&c, true);
          dma_channel_configure(
              disk_dma_chan, &c,
              dst + disk_offset,     // 書き込み先（RAMディスク）
              &memory[dma_addr_z80], // 読み出し元（Z80メモリ）
              128, true);
          fdc_status = 0;
        } else {
          fdc_status = 1;
        }
      }
    }
    break;
  }
  case 0x0F: // 15:0x0F : DMAアドレス
    dma_addr_low = data_byte;
    break;
  case 0x10: // 16:0x10 : DMAアドレス
    dma_addr_high = data_byte;
    break;
  case 0x30: // 0x30 : PPI PA
    // ==== ここから先はSIO直叩き（最速） ====
    if (data_byte & 1) {
      sio_hw->gpio_set = (1u << PA0_PIN); // PA b0 ON (GPIO27)
    } else {
      sio_hw->gpio_clr = (1u << PA0_PIN); // PA b0 OFF (GPIO27)
    }
    break;
  default:
    break;
  }
}

uint8_t __attribute__((noinline)) handle_io_read(uint32_t ioadrs,
                                                 uint32_t agpio) {
  uint8_t data_byte = 0;
  switch (ioadrs) {
  case 0x00:               // === ポート0 : CONSTA ===
    data_byte = uart_stat; // 0:not ready, 0xFF:ready
    break;
  case 0x01: // === ポート1 : CONIN ===
    data_byte = uart_rxdata;
    uart_stat = 0;
    break;
  case 0x09: // DMA完了ステータスポート
    if (dma_busy && dma_channel_is_busy(disk_dma_chan)) {
      data_byte = 0xFF; // まだ転送中（Busy）
    } else {
      data_byte = 0x00; // 転送完了（Ready）
      dma_busy = false;
    }
    break;
  case 0x0E: // 14:0x0E : FDCステータス(0:OK/1:NG)
    data_byte = fdc_status;
    break;
  default:
    data_byte = (uint8_t)(agpio >> DATA_BASE);
    break;
  }
  return data_byte;
}

// --- Main Emulation Loop ---
// __attribute__((noinline)) void __time_critical_func(emu_loop)(void) {
void __time_critical_func(emu_loop)(void) {
  init_disk_dma(); // DMAC 初期化

  // PIO レジスタ・マスク・ポインタをキャッシュ（ループ外で1回だけ）
  uint8_t *const mem_ptr = memory;
  volatile uint32_t *rxf = (volatile uint32_t *)&pio0_hw->rxf[sm_emu];
  //  volatile uint32_t *txf = (volatile uint32_t *)&pio0_hw->txf[sm_emu];
  volatile uint32_t *txf = (volatile uint32_t *)&pio1_hw->txf[sm_emu];
  const uint32_t rxempty_mask = 1u << (PIO_FSTAT_RXEMPTY_LSB + sm_emu);

  // MREQ(24) と WR(26) のビットだけを抽出するマスク
  const uint32_t bus_mask = (1u << MREQ_PIN) | (1u << WR_PIN);
  const uint32_t mem_read_state = (1u << WR_PIN); // MREQ=0, WR=1
  const uint32_t mem_write_state = 0;             // MREQ=0, WR=0
                                                  //  uint32_t count = 0;

  while (true) {
    // ① PIO RX FIFO 直叩き（SDK関数バイパス）
    while (pio0_hw->fstat & rxempty_mask)
      tight_loop_contents();
    uint32_t agpio = *rxf;

    // 状態を一括抽出 (1 cycle)
    uint32_t bus_state = agpio & bus_mask;

    // ② 圧倒的高頻度の Memory-Read を最速の直線パスにする
    if (__builtin_expect(bus_state == mem_read_state, 1)) {
      *txf = mem_ptr[(uint16_t)agpio]; // Memory-Read
    } else if (bus_state == mem_write_state) {
      mem_ptr[(uint16_t)agpio] = (uint8_t)(agpio >> DATA_BASE); // Memory-Write
    } else {
      // MREQ=1 (I/Oアクセス)
      clk_pwm_output_off(); // Z80クロック停止
      uint ioadrs = agpio & 0xFF;

      if (!(agpio & (1u << WR_PIN))) { // MREQ=1, WR=0  I/O-Write
        uint8_t data_byte = (uint8_t)(agpio >> DATA_BASE);
        handle_io_write(ioadrs, data_byte);
      } else { // I/O-Read
        *txf = handle_io_read(ioadrs,
                              agpio); // TXF 直接書き込み（Full チェック不要）
      }
      clk_pwm_output_on(); // Z80クロック再開
    }
#if 0
    if (true) { // デバッグ用 Z80_freq = 20  (20Hz) で使用する
      printf("%05u MREQ:%d WR:%d RD:%d ADRS:%04X DATA:%02X\n", count,
             (agpio >> MREQ_PIN) & 1, (agpio >> WR_PIN) & 1,
             (agpio >> RD_PIN) & 1, (uint16_t)agpio, (uint8_t)(agpio >> DATA_BASE));
      count++;
    }
#endif
  }
}

//
//  メイン関数
//
int main() {
  uint32_t sysclk = clock_get_hz(clk_sys);
  int sysvolt = vreg_get_voltage();

  if (false) { // 高速 コア電圧 クロック 設定
    sleep_ms(0);
    sysvolt = VREG_VOLTAGE_1_30;
    // sysvolt = VREG_VOLTAGE_1_25;
    vreg_set_voltage(sysvolt);
    sleep_ms(100);
    // sysclk = 400000;
    // sysclk = 360000;
    // sysclk = 336000;
    // sysclk = 312000;
    // sysclk = 300000; // 300MHz 1.3V Z80 10MHz
    sysclk = 288000;
    // sysclk = 276000;  // 276MHz 1.25V Z80 9MHz
    // sysclk = 264000;
    // sysclk = 200000;
    if (set_sys_clock_khz(sysclk, true)) {
#if PICO_RP2040
      // ssi_hw->baudr = 2; // 400MHz / 2 = 200MHz
      ssi_hw->baudr = 3; // 300MHz / 3 = 100MHz
                         // ssi_hw->baudr = 4; // 300MHz / 4 = 75MHz
                         // ssi_hw->baudr = 5; // 300MHz / 5 = 60MHz
#endif
    }
  } else { // 標準　コア電圧 1.10V クロック 200MHz 設定
    sleep_ms(0);
    sysvolt = VREG_VOLTAGE_1_10;
    vreg_set_voltage(sysvolt);
    sleep_ms(100);
    sysclk = 200000;
    if (set_sys_clock_khz(sysclk, true)) {
#if PICO_RP2040
      // ssi_hw->baudr = 2; // 200MHz / 2 = 100MHz
      ssi_hw->baudr = 3; // 200MHz / 3 = 66MHz
#endif
    }
  }

  sleep_ms(0);
  stdio_init_all();
  sleep_ms(0);

  // // Z80用メモリー初期化
  memcpy(memory + 0xE400, ccp_bdos, ccp_bdos_size);
  memcpy(memory + 0xFA00, bios01, bios01_size);
  memcpy(memory, boot, sizeof(boot));

  // GPIO初期化
  // 入力: A0-A15,D0-D7,IORQ,MREQ,RD,WR,RFSH
  for (int i = 0; i < 16; i++) {
    gpio_init(ADRS_BASE + i);
    gpio_set_dir(ADRS_BASE + i, GPIO_IN);
    //   gpio_pull_up(i);
  }
  for (int i = 0; i < 8; i++) {
    gpio_init(DATA_BASE + i);
    gpio_set_dir(DATA_BASE + i, GPIO_IN);
    //   gpio_pull_up(i);
  }
  // gpio_init( IORQ_PIN);
  // gpio_set_dir(IORQ_PIN, GPIO_IN);
  gpio_init(MREQ_PIN);
  gpio_set_dir(MREQ_PIN, GPIO_IN);
  gpio_init(RD_PIN);
  gpio_set_dir(RD_PIN, GPIO_IN);
  gpio_init(WR_PIN);
  gpio_set_dir(WR_PIN, GPIO_IN);

  // 出力: RESET
  gpio_init(RESET_PIN);
  gpio_set_dir(RESET_PIN, GPIO_OUT);
  gpio_put(RESET_PIN, 0); // RESET ON

  // ====================== GPIO初期設定はC SDKで（超簡単・安全）
  // ======================
  gpio_init(PA0_PIN);              // ピン初期化（FUNCSEL = SIOに自動設定）
  gpio_set_dir(PA0_PIN, GPIO_OUT); // 出力方向に設定（SIOのOEも自動でON）
  gpio_put(PA0_PIN, 0);            // 初期値はOFF（任意）

  // printf("GPIO 初期設定完了（SDK使用）→ 以後SIO直叩きでON/OFF\n");
  sleep_ms(0);

  // Initial CLK pulses (Python: CLK_OnOff(10))
  clk_on_off(10);
  // PIO初期化
  pio_init_bus();

  sleep_ms(1000);
  // EMUZ80_RP2040_PCB
  printf("\n** For EMUZ80_RP2040_PCB! **\n");
  printf(
      "** z80pack - CP/M2.2 CCP+BDOS(E400H-F9FFH), BIOS-01(FA00H-FC2FH) **\n");
  printf("** DISK0 A: z80pack cpm2-1.dsk   **\n");
  printf("** DISK1 B: cpm22_disk1.dsk      **\n");
  printf("** DISK2 C: cpm22_tp301a.dsk     **\n");
  printf("** DISK3 D: cpm22_z80forth.dsk   **\n");
  printf("** DISK8 I: cpm22_htc.dsk(650KB) **\n");
  printf("** DISK9 J: RAMDISK (128KB)      **\n");

  printf("\n-hit [Enter] in terminal-\n");
  while (getchar_timeout_us(100) == PICO_ERROR_TIMEOUT)
    ;
  printf("\nfor CP/M2.2 v1.1\n");

  float volt = 0;
  if (sysvolt == VREG_VOLTAGE_1_10)
    volt = 1.10;
  else if (sysvolt == VREG_VOLTAGE_1_15)
    volt = 1.15;
  else if (sysvolt == VREG_VOLTAGE_1_20)
    volt = 1.20;
  else if (sysvolt == VREG_VOLTAGE_1_25)
    volt = 1.25;
  else if (sysvolt == VREG_VOLTAGE_1_30)
    volt = 1.30;

  //  エミュレーション開始(core1)
  printf("AE-RP2040 Core:%0.2fV Clock:%uMHz (I/O CLK-STOP)\n", volt,
         sysclk / 1000);
  printf("Emulation task(core1) Start..\n");

  multicore_launch_core1(emu_loop);
  sleep_ms(0);

  // CLK PWM Setup ,  MAX RP2040 1.3v 288MHz Z80 9MHz
  // int Z80_freq = 12000000; // 12MHz
  // int Z80_freq = 11000000; // 11MHz
  // int Z80_freq = 10000000; // 10MHz
  // int Z80_freq = 9000000; // 9MHz
  // int Z80_freq = 8000000; // 8MHz
  // int Z80_freq = 7000000; // 7MHz
  int Z80_freq = 6000000; // 6MHz
  // int Z80_freq = 5000000; // 5MHz
  // int Z80_freq = 4000000; // 4MHz
  // int Z80_freq = 2500000; // 2.5MHz
  // int Z80_freq = 1000000; // 1MHz
  // int Z80_freq = 800000; // 700kHz
  // int Z80_freq = 700000; // 700kHz
  // int Z80_freq = 600000; // 600kHz
  // int Z80_freq = 500000; // 500kHz
  // int Z80_freq = 400000; // 400kHz
  // int Z80_freq = 300000; // 300kHz
  // int Z80_freq = 200000; // 200kHz
  // int Z80_freq = 150000; // 150kHz
  // int Z80_freq = 100000; // 100kHz
  // int Z80_freq = 10000; // 10kHz
  //  int Z80_freq = 20; // 20Hz
  init_clk_pwm(Z80_freq);
  printf("Z80 CLK-ON %fMHz\n", Z80_freq / 1000000.0);

  // 0.1秒後にRESETを解除
  add_alarm_in_ms(100, reset_off_callback, NULL, false);

  printf("main task1(Core0) start..\n");
  task1();

  // Cleanup
  gpio_put(RESET_PIN, 0);
  printf("RESET-ON\n");
  sleep_ms(100);
  clk_pwm_output_off();
  clk_on_off(10);
  printf("Exited.\n");

  return 0;
}
