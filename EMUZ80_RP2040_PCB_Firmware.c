// EMUZ80_RP2040_PCB_Firmware: Z80 bus emulator for Akizuki AE-RP2040
// ** For EMUZ80_RP2040_PCB! **
// ** ROM-DATA: EMUBASIC_IO  **

#include "AE-RP2040.pio.h"
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

// #define MEMORY_SIZE 2048
#define MEMORY_SIZE 65536 // 64KB

// static uint8_t memory[MEMORY_SIZE];
static uint8_t __attribute__((aligned(4))) memory[MEMORY_SIZE];
volatile bool stop_flg = false;

// UART/USB 共有バッファ
#define UART_TX_BUF_SIZE 256

volatile uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
volatile uint16_t uart_tx_head = 0; // コア1 (Z80側) が更新
volatile uint16_t uart_tx_tail = 0; // コア0 (UART側) が更新

volatile uint8_t uart_rxdata = 0;
volatile uint8_t uart_stat = 0;

#define UART_RX_READY 0xFF
// #define UART_TX_READY   0x02
// volatile uint8_t uart_txdata = 0;
// volatile uint8_t uart_rxdata = 0;
// volatile uint8_t uart_stat = 1;

// Test Program (from Python testprg2)
const uint8_t testprg2[] = {0x21, 0x00, 0x00,  // LD HL, 0000
                            0x22, 0x00, 0x80,  // LD (8000), HL
                            0x23,              // INC HL
                            0xC3, 0x03, 0x00}; // JP 0003

// ROM-BASIC (EMUZ80のEMUBASIC)
// @tendai22plusさんによる UART I/Oアクセス改造版
// #define EMUBASIC_IO
// #include "emubasic_io.h"

// const uint8_t boot[] = {
//    0xC3, 0x18, 0x00, 0x42, 0x4F, 0x4F, 0x54, 0x3A,
//    0x20, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, // 0x0000
//    0x6F, 0x72, 0x6C, 0x64, 0x21, 0x0D, 0x0A, 0x00,
//    0x31, 0x80, 0x00, 0x21, 0x03, 0x00, 0xCD, 0x33, // 0x0010
//    0xFB, 0xCD, 0x06, 0xFA, 0xB7, 0xCA, 0x21, 0x00,
//    0xCD, 0x09, 0xFA, 0x4F, 0xCD, 0x0C, 0xFA, 0xC3, // 0x0020
//    0x21, 0x00, 0x76,                               // 0x0030
//};

// const unsigned char boot[] = {
//     0xC3, 0x18, 0x00, 0x42, 0x4F, 0x4F, 0x54, 0x3A,
//     0x20, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, // 0x00000000
//     0x6F, 0x72, 0x6C, 0x64, 0x21, 0x0D, 0x0A, 0x00,
//     0x31, 0x80, 0x00, 0x21, 0xCB, 0xFA, 0xCD, 0x33, // 0x00000010
//     0xFB, 0x76,                                     // 0x00000020
// };

const unsigned char boot[] = {
    0xC3,
    0x00,
    0xFA, // JP BIOS
};
const size_t boot_size = sizeof(boot);

// z80pack CP/M2.2 CCP/BDOS
#include "ccp_bdos.h"

// z80pack BIOS-01
#include "bios01.h"

// ====================== 仮想ディスク定義 ======================
// A: 仮想ROMディスク (Read Only) - cpm22-1.dsk をそのまま埋め込む
// z80pack cpm2-src-1.dsk
// #include "cpm2-src-1.h"

// z80pack cpm2-1.dsk
#include "cpm22-1.h"

// B: 仮想RAMディスク (Read/Write) - 十分なサイズを確保
#define RAMDISK_SIZE (64 * 1024) // 64KB（必要に応じて128KBまで拡大可）
static uint8_t ramdisk[RAMDISK_SIZE] = {0xE5};

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
  printf("RESET-OFF (Delayed 1s)\n\n");
  gpio_put(RESET_PIN, 1); // RESET-OFF (High)
  return 0;               // ONE_SHOT
}

// --- Helper: Set PWM Frequency in Hz (Integer only) ---
void set_pwm_freq(uint pin, uint32_t freq) {
  uint slice_num = pwm_gpio_to_slice_num(pin);
  uint32_t sys_clk = clock_get_hz(clk_sys);

  float clkdiv = 1.0f;
  uint32_t wrap = (sys_clk / freq) - 1;

  if (wrap > 65535) {
    // wrap が最大値を越える場合、clkdivを調整
    clkdiv = (float)sys_clk / (freq * 65536LL);
    if (clkdiv > 255.9375f) {
      clkdiv = 255.9375f;
    }
    wrap = (uint32_t)((float)sys_clk / (freq * clkdiv)) - 1;
    if (wrap > 65535) {
      wrap = 65535;
    }
  }

  pwm_set_clkdiv(slice_num, clkdiv);
  pwm_set_wrap(slice_num, wrap);
  pwm_set_gpio_level(pin, (wrap + 1) / 2);
}

uint sm_trg = 0;
uint sm_emu = 1;
uint sm_dirsL = 2;
uint sm_dirsH = 3;

// --- PIO Helpers ---
void pio_init_bus() {
  PIO pio = pio0;

  // SM0: trg_rw2 (Detection of falling edge on RD/WR)
  uint offset_trg = pio_add_program(pio, &trg_rw2_program);
  pio_sm_config c_trg = trg_rw2_program_get_default_config(offset_trg);

  // SM1: m_emu (Address/Data handling)
  uint offset_emu = pio_add_program(pio, &m_emu_program);
  pio_sm_config c_emu = m_emu_program_get_default_config(offset_emu);

  // SM2/3: d_pindirs (Direction toggle)
  uint offset_dirs = pio_add_program(pio, &d_pindirs_program);
  pio_sm_config c_dirs = d_pindirs_program_get_default_config(offset_dirs);
  pio_sm_config c_dirsH = d_pindirs_program_get_default_config(offset_dirs);

  // GPIOをPIO用に初期化 GP0-15(A0-15), GP16-23(D0-7), GP24(MREQ/IORQ),
  // GP25(RD), GP26(WR), GP27(WAIT), GP28(RESET), GP29(CLK)
  for (int i = 0; i <= 23; i++) {
    pio_gpio_init(pio, i);
  }
  //  pio_gpio_init(pio, IORQ_PIN); // GP24(IORQ)
  pio_gpio_init(pio, MREQ_PIN); // GP24(MREQ)
  pio_gpio_init(pio, RD_PIN);   // GP25(RD)
  pio_gpio_init(pio, WR_PIN);   // GP26(WR)
  //  pio_gpio_init(pio, WAIT_PIN); // GP27(WAIT)

  // SM0: trg_rw2 (Detection of falling edge on RD/WR)
  sm_config_set_in_pins(&c_trg, RD_PIN); // base = GP25 (RD, WR)
  pio_sm_init(pio, sm_trg, offset_trg, &c_trg);
  pio_sm_set_enabled(pio, sm_trg, true);

  // SM1: m_emu (Address/Data handling)
  sm_config_set_in_pins(&c_emu, 0);             // base = GP0
  sm_config_set_out_pins(&c_emu, DATA_BASE, 8); // base = GP16, cnt = 8
  sm_config_set_jmp_pin(&c_emu, RD_PIN);

  // D0-7ピン初期化(入力)
  pio_sm_set_consecutive_pindirs(pio, sm_emu, DATA_BASE, 8, false);
  // シフトレジスタの設定 (Auto Push/Pull 有効化)
  // ISRのシフト方向, auto_push=true, threshold=30
  sm_config_set_in_shift(&c_emu, false, true, 30);
  // OSRのシフト方向, auto_pull=true, threshold=8
  sm_config_set_out_shift(&c_emu, true, true, 8);

  pio_sm_init(pio, sm_emu, offset_emu, &c_emu);
  pio_sm_set_enabled(pio, sm_emu, true);

  // SM2/3: d_pindirs (Direction toggle)
  sm_config_set_set_pins(&c_dirs, DATA_BASE, 4); // GP0..3
  sm_config_set_jmp_pin(&c_dirs, RD_PIN);
  pio_sm_init(pio, sm_dirsL, offset_dirs, &c_dirs);

  sm_config_set_set_pins(&c_dirsH, DATA_BASE + 4, 4); // GP4..7
  sm_config_set_jmp_pin(&c_dirsH, RD_PIN);
  pio_sm_init(pio, sm_dirsH, offset_dirs, &c_dirsH);

  pio_sm_set_enabled(pio, sm_dirsL, true);
  pio_sm_set_enabled(pio, sm_dirsH, true);
}

// --- UART Task (Core 0) ---
void task1(void) {
  printf("task1 UART start..\n");
  while (true) {
    // 送信処理 (Z80 -> USB)
    //    if ((uart_stat & 0x02) == 0) {
    //      if (tud_cdc_connected() && tud_cdc_write_available() > 0) {
    //        putchar(uart_txdata);
    //        uart_stat |= 0x02; // TX Buffer Empty
    //      }
    //    }
    while (uart_tx_head != uart_tx_tail) {

      //        if (uart_is_writable()) {                // UART or
      //        CDCが送信可能
      if (tud_cdc_connected() && tud_cdc_write_available() > 0) {
        uint8_t ch = uart_tx_buf[uart_tx_tail];
        //        tud_cdc_write_char(ch); // USB CDCを使う場合はこちら
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
        if (c == 0x04) { // Ctrl-D: Stop emulation
          printf("\ntask1: Ctrl-D detected. Stopping..\n");
          stop_flg = true;
          break;
        }
        uart_rxdata = (uint8_t)c;
        uart_stat = 0xFF; // RX Data Available
      }
    }
    sleep_ms(1);
  }
}

// uint32_t単位でコピー（RP2040のM0+にかなり効く）
void __time_critical_func(fast_copy_128)(uint8_t *dst, const uint8_t *src) {
  uint32_t *d = (uint32_t *)dst;
  const uint32_t *s = (const uint32_t *)src;
  for (int i = 0; i < 32; i++) { // 128/4 = 32
    *d++ = *s++;
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

// --- Main Emulation Loop ---
__attribute__((noinline)) void __time_critical_func(emu_loop)(void) {
  PIO pio = pio0;
  uint count = 0;
  uint8_t data_byte = 0;

  uint8_t current_drive = 0;
  uint8_t current_track = 0;
  uint8_t current_sector = 0;
  uint8_t read_write = 0;
  uint8_t fdc_status = 0;
  uint8_t dma_addr_low = 0;
  uint8_t dma_addr_high = 0;

  init_disk_dma(); // DMAC 初期化
  while (true) {
    // バスライン取得
    // GP0-29(A0-15=GP0-15,D0-7=GP16-23,MREQ=GP24,RD=GP25,WR=GP26,WAIT=GP27,RESET=GP28,CLK=GP29)
    const uint32_t mreq_mask = (1u << MREQ_PIN);
    const uint32_t wr_mask = (1u << WR_PIN);
    uint32_t agpio = pio_sm_get_blocking(pio, sm_emu);
    uint32_t adrs_word = (agpio & 0xFFFF);

    count++;
    if (!(agpio & mreq_mask)) { // MREQ=0 メモリアクセス
      if (!(agpio & wr_mask)) { // MREQ=0, WR=0  Memory-Write
        data_byte = (uint8_t)(agpio >> DATA_BASE);
        //        if (adrs_word >= 0x8000) {
        memory[adrs_word] = data_byte;
        //        }
      } else { // MREQ=0, WR=1  Memory-Read (not Write)
        data_byte = memory[adrs_word];
        pio_sm_put_blocking(pio, sm_emu, data_byte);
      }
    } else { // MREQ=1  I/Oアクセス
      uint ioadrs = adrs_word & 0xFF;
      if (!(agpio & wr_mask)) { // MREQ=1, WR=0  I/O-Write (not Memory-access)
        data_byte = (uint8_t)(agpio >> DATA_BASE);
        if (ioadrs == 0x01) { // CONOUT : ポート1
          uint8_t next = (uart_tx_head + 1) % UART_TX_BUF_SIZE;
          if (next != uart_tx_tail) {
            uart_tx_buf[uart_tx_head] = data_byte;
            uart_tx_head = next;
          }
          // 満杯時は文字を捨てる
        } else if (ioadrs == 0x0A) { // 10:0x0A : ドライブ選択
          current_drive = data_byte;
        } else if (ioadrs == 0x0B) { // 11:0x0B : トラック選択
          current_track = data_byte;
        } else if (ioadrs == 0x0C) { // 12:0x0C : セクタ選択
          current_sector = data_byte;
        } else if (ioadrs == 0x0D) { // 13:0x0D:FDCOPコマンド(0=Read,1=Write)
          // ------------------------------------------------------------------
          read_write = data_byte;

          uint16_t dma_addr_z80 = ((uint16_t)dma_addr_high << 8) | dma_addr_low;
          uint8_t logical_sector = current_sector;
          if (logical_sector >= 1)
            logical_sector--; // 1-based → 0-based

          // 乗算を少し最適化（*26 = *16 + *8 + *2）
          uint32_t disk_offset = ((uint32_t)current_track << 4) +
                                 ((uint32_t)current_track << 3) +
                                 ((uint32_t)current_track << 1);
          disk_offset = (disk_offset + logical_sector) * 128UL;

          if (read_write == 0) { // ============== READ ==============
            const uint8_t *src = NULL;
            uint32_t max_size = 0;

            if (current_drive == 0) { // A: ROM
              src = romdisk;
              max_size = ROMDISK_SIZE;
            } else if (current_drive == 1) { // B: RAM
              src = ramdisk;
              max_size = RAMDISK_SIZE;
            }
            if (src && disk_offset + 128 <= max_size && disk_dma_chan >= 0) {
              // 前のDMAがまだ動いていたら強制終了（安全策）
              if (dma_channel_is_busy(disk_dma_chan)) {
                dma_channel_abort(disk_dma_chan);
              }
              // uint32_t start = time_us_32(); // 時間測定
              // DMAで128バイトコピー開始（即時）
              dma_channel_config c =
                  dma_channel_get_default_config(disk_dma_chan);
              channel_config_set_transfer_data_size(&c,
                                                    DMA_SIZE_8); // 8bit単位
              channel_config_set_read_increment(&c, true);
              channel_config_set_write_increment(&c, true);
              // channel_config_set_dreq(&c, 0); // 常に即時（デフォルト）
              dma_channel_configure(
                  disk_dma_chan, &c,
                  &memory[dma_addr_z80], // 書き込み先（Z80メモリ）
                  src + disk_offset,     // 読み出し元
                  128,                   // 転送数（バイト）
                  true);                 // 即開始
              // 転送終了待ちする場合
              // dma_channel_wait_for_finish_blocking(disk_dma_chan);
              // uint32_t end = time_us_32();    // 時間測定
              // printf("Read 128B: %u us\n", end - start);
              dma_busy = true; // DMA開始
              fdc_status = 0;  // 即OK返却（DMAはバックグラウンド）
            } else {
              memset(&memory[dma_addr_z80], 0xE5, 128);
              fdc_status = 1;
            }

          } else { // ================== WRITE ==================
            uint8_t *dst = NULL;
            uint32_t max_size = 0;
            if (current_drive == 0) { // A: ROM → 書き込み禁止
              fdc_status = 1;
            } else if (current_drive == 1) { // B: RAM
              dst = ramdisk;
              max_size = RAMDISK_SIZE;

              if (dst && disk_offset + 128 <= max_size && disk_dma_chan >= 0) {
                dma_channel_config c =
                    dma_channel_get_default_config(disk_dma_chan);
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
        }

        // ---------------------------------------
        //          read_write = data_byte;
        //          // if (read_write == 0) { // DISK READ
        //          //   memset(memory + ((dma_addr_high << 8) |
        //          dma_addr_low), 0xE5,
        //          //   128);
        //          // }
        //
        //          uint16_t dma_addr = ((uint16_t)dma_addr_high << 8) |
        //          dma_addr_low; uint8_t logical_sector = current_sector; if
        //          (logical_sector >= 1)
        //            logical_sector--; // 1-based → 0-based
        //
        //          uint32_t disk_offset =
        //              ((uint32_t)current_track * 26UL + logical_sector) *
        //              128UL;
        //
        //          if (read_write == 0) { // READ
        //            uint8_t *src = NULL;
        //            uint32_t max_size = 0;
        //
        //            if (current_drive == 0) { // A: ROM
        //              src = (uint8_t *)romdisk;
        //              max_size = ROMDISK_SIZE;
        //            } else if (current_drive == 1) { // B: RAM
        //              src = ramdisk;
        //              max_size = RAMDISK_SIZE;
        //            }
        //
        //            if (src && disk_offset + 128 <= max_size) {
        //              // uint32_t start = time_us_32(); // **** 時間測定
        //              **** memcpy(&memory[dma_addr], src + disk_offset,
        //              128);
        //              // fast_copy_128(&memory[dma_addr], src +
        //              disk_offset);
        //              //
        //              // 後で高速版に置き換え可
        //              // uint32_t end = time_us_32();
        //              // printf("Read 128B: %u us\n", end - start);
        //              fdc_status = 0; // OK
        //            } else {
        //              memset(&memory[dma_addr], 0xE5, 128);
        //             fdc_status = 1; // Error
        //          }
        //
        //          } else { // WRITE
        //            uint8_t *dst = NULL;
        //            uint32_t max_size = 0;
        //
        //            if (current_drive == 0) { // A: ROM → 書き込み禁止
        //              fdc_status = 1;
        //            } else if (current_drive == 1) { // B: RAM
        //              dst = ramdisk;
        //              max_size = RAMDISK_SIZE;
        //              if (disk_offset + 128 <= max_size) {
        //                memcpy(dst + disk_offset, &memory[dma_addr], 128);
        //                // fast_copy_128(dst + disk_offset,
        //                &memory[dma_addr]); fdc_status = 0;
        //              } else {
        //                fdc_status = 1;
        //              }

        else if (ioadrs == 0x0F) { // 15:0x0F : DMAアドレス
          dma_addr_low = data_byte;
        } else if (ioadrs == 0x10) { // 16:0x10 : DMAアドレス
          dma_addr_high = data_byte;
        } else if (ioadrs == 0x30) { // 0x30 : PPI PA
          // ==== ここから先はSIO直叩き（最速） ====
          if (data_byte & 1) {
            sio_hw->gpio_set = (1u << PA0_PIN); // PA b0 ON (GPIO27)
          } else {
            sio_hw->gpio_clr = (1u << PA0_PIN); // PA b0 OFF (GPIO27)
          }
        }
      } else {                   // MREQ=1, WR=1  I/O-Read (not Memory-access)
        if (ioadrs == 0x00) {    // === ポート0 : CONSTA ===
          data_byte = uart_stat; // 0:not ready, 0xFF:ready
        } else if (ioadrs == 0x01) { // === ポート1 : CONIN ===
          data_byte = uart_rxdata;
          uart_stat = 0;
        } else if (ioadrs == 0x09) { // ← 新規追加：DMA完了ステータスポート
          if (dma_busy && dma_channel_is_busy(disk_dma_chan)) {
            data_byte = 0xFF; // まだ転送中（Busy）
          } else {
            data_byte = 0x00; // 転送完了（Ready）
            dma_busy = false;
          }
        } else if (ioadrs == 0x0E) { // 14:0x0E : FDCステータス(0:OK/1:NG)
          data_byte = 0;
        } else {
          data_byte = (uint8_t)(agpio >> DATA_BASE);
        }
        pio_sm_put_blocking(pio, sm_emu, data_byte);
      }
    }
    if (false) { // デバッグ用 Z80_freq = 20  (20Hz) で使用する
      printf("%05u MREQ:%d WR:%d RD:%d ADRS:%04X DATA:%02X\n", count,
             (agpio >> MREQ_PIN) & 1, (agpio >> WR_PIN) & 1,
             (agpio >> RD_PIN) & 1, adrs_word, (int)data_byte);
    }
  }
}

//
//  メイン関数
//
int main() {
  uint32_t sysclk = clock_get_hz(clk_sys);
  int sysvolt = VREG_VOLTAGE_1_15;

  if (true) { // 高速 コア電圧1.3V クロック 360/400MHz 設定
    sleep_ms(100);
    sysvolt = VREG_VOLTAGE_1_30;
    vreg_set_voltage(sysvolt);
    sleep_ms(100);
    // sysclk = 400000;
    // sysclk = 360000; // baudr = 4
    // sysclk = 340000; // baudr = 3
    // sysclk = 320000; // baudr = 3
    sysclk = 300000; // baudr = 3
    // sysclk = 280000; // baudr = 3
    // sysclk = 260000; // baudr = 2
    // sysclk = 200000;
    if (set_sys_clock_khz(sysclk, true)) {
#if PICO_RP2040
      // ssi_hw->baudr = 2; // 400MHz / 3 = 133MHz
      ssi_hw->baudr = 3; // 400MHz / 3 = 133MHz
                         // ssi_hw->baudr = 4; // 400MHz / 3 = 133MHz
#endif
    }
  } else { // 標準　コア電圧 1.15V クロック 200MHz 設定
    sleep_ms(100);
    //    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sysvolt = VREG_VOLTAGE_1_15;
    vreg_set_voltage(sysvolt);
    sleep_ms(100);
    sysclk = 200000;
    if (set_sys_clock_khz(sysclk, true)) {
#if PICO_RP2040
      ssi_hw->baudr = 2; // 200MHz / 2 = 100MHz
#endif
    }
  }

  sleep_ms(100);
  stdio_init_all();
  sleep_ms(100);

  // Z80用メモリー初期化
  memset(memory, 0xFF, MEMORY_SIZE);
  memcpy(memory + 0xE400, ccp_bdos, sizeof(ccp_bdos));
  //  memcpy(memory + 0xFA00, bios, sizeof(bios));
  memcpy(memory + 0xFA00, bios01, sizeof(bios01));
  memcpy(memory, boot, sizeof(boot));

  // GPIO初期化 GP0-29
  // A0-A15:GP0-15,D0-D7:GP16-23,IORQ:GP24,MREQ:GP24,RD:GP25,WR:GP26,WAIT:GP27,RESET:GP28,CLK:GP29
  gpio_init_mask(0x0FFFFFFF);
  for (int i = 0; i <= 23; i++) {
    gpio_set_dir(i, GPIO_IN);
    //   gpio_pull_up(i);
  }

  // 他の制御ピン RESET:GP28 CLK:GP29
  gpio_init(RESET_PIN);
  gpio_set_dir(RESET_PIN, GPIO_OUT);
  gpio_put(RESET_PIN, 0); // RESET ON

  // ====================== GPIO初期設定はC SDKで（超簡単・安全）
  // ======================
  gpio_init(PA0_PIN); // ピン初期化（FUNCSEL = SIOに自動設定）GPIO27
  gpio_set_dir(PA0_PIN, GPIO_OUT); // 出力方向に設定（SIOのOEも自動でON）
  gpio_put(PA0_PIN, 0);            // 初期値はOFF（任意）

  // printf("GPIO27 初期設定完了（SDK使用）→ 以後SIO直叩きでON/OFF\n");
  sleep_ms(100);

  // Initial CLK pulses (Python: CLK_OnOff(10))
  clk_on_off(10);
  // PIO初期化
  pio_init_bus();

  sleep_ms(2000);
  // EMUZ80_RP2040_PCB
  printf("\n** For EMUZ80_RP2040_PCB! **\n");
  printf("** z80pack: CP/M2.2 CCP+BDOS(E400H-F9FFH), BIOS-01(FA00H-FC2FH), "
         "BOOT(0000H-) **\n");
  printf("** DISK: z80pack cpm2-1.dsk **");
  printf("\n-hit [Enter] in terminal-\n");
  while (getchar_timeout_us(100) == PICO_ERROR_TIMEOUT)
    ;
  float volt = 0;
  if (sysvolt == VREG_VOLTAGE_1_15)
    volt = 1.15;
  if (sysvolt == VREG_VOLTAGE_1_30)
    volt = 1.30;

  //  エミュレーション開始(core1)
  printf("\nAE-RP2040 Core:%0.2fV Clock:%uMHz\n", volt, sysclk / 1000);
  printf("Emulation task(core1) Start..\n");

  multicore_launch_core1(emu_loop);
  sleep_ms(1000);

  // CLK PWM Setup ,  MAX RP2040 400MHz Z80 11MHz
  // int Z80_freq = 12000000; // 12MHz
  // int Z80_freq = 11000000; // 11MHz
  // int Z80_freq = 10000000; // 10MHz
  // int Z80_freq = 9000000; // 9MHz
  // int Z80_freq = 8000000; // 8MHz
  // int Z80_freq = 7000000; // 7MHz
  int Z80_freq = 6000000; // 6MHz
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
  gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);
  uint slice_num = pwm_gpio_to_slice_num(CLK_PIN);
  set_pwm_freq(CLK_PIN, Z80_freq);
  pwm_set_enabled(slice_num, true);
  printf("Z80 CLK-ON %fMHz\n", Z80_freq / 1000000.0);

  // 1秒後にRESETを解除
  add_alarm_in_ms(1000, reset_off_callback, NULL, false);

  printf("main task1(Core0) start..\n");
  task1();

  // Cleanup
  gpio_put(RESET_PIN, 0);
  printf("RESET-ON\n");
  sleep_ms(100);
  pwm_set_enabled(slice_num, false);
  clk_on_off(10);
  printf("Exited.\n");

  return 0;
}
