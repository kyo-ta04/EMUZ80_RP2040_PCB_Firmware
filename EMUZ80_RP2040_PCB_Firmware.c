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

static uint8_t memory[MEMORY_SIZE];
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

// z80pack CP/M2.2 64KB BIOS address FA00-FC2F
#define BIOS_BASE 0xFA00
const unsigned char bios[] = {
    0xC3, 0x3E, 0xFB, 0xC3, 0x51, 0xFB, 0xC3, 0xBC,
    0xFB, 0xC3, 0xBF, 0xFB, 0xC3, 0xC2, 0xFB, 0xC3, // 0x0000FA00
    0xC6, 0xFB, 0xC3, 0xCD, 0xFB, 0xC3, 0xD1, 0xFB,
    0xC3, 0xD4, 0xFB, 0xC3, 0xD9, 0xFB, 0xC3, 0x05, // 0x0000FA10
    0xFC, 0xC3, 0x09, 0xFC, 0xC3, 0x1E, 0xFC, 0xC3,
    0x25, 0xFC, 0xC3, 0x29, 0xFC, 0xC3, 0xCA, 0xFB, // 0x0000FA20
    0xC3, 0x0D, 0xFC, 0x73, 0xFA, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0xFC, 0x8D, 0xFA, 0x2A, // 0x0000FA30
    0xFF, 0xB0, 0xFC, 0x73, 0xFA, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0xFC, 0x8D, 0xFA, 0x3A, // 0x0000FA40
    0xFF, 0xCF, 0xFC, 0x73, 0xFA, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0xFC, 0x8D, 0xFA, 0x4A, // 0x0000FA50
    0xFF, 0xEE, 0xFC, 0x73, 0xFA, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0xFC, 0x8D, 0xFA, 0x5A, // 0x0000FA60
    0xFF, 0x0D, 0xFD, 0x01, 0x07, 0x0D, 0x13, 0x19,
    0x05, 0x0B, 0x11, 0x17, 0x03, 0x09, 0x0F, 0x15, // 0x0000FA70
    0x02, 0x08, 0x0E, 0x14, 0x1A, 0x06, 0x0C, 0x12,
    0x18, 0x04, 0x0A, 0x10, 0x16, 0x1A, 0x00, 0x03, // 0x0000FA80
    0x07, 0x00, 0xF2, 0x00, 0x3F, 0x00, 0xC0, 0x00,
    0x10, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x0000FA90
    0x00, 0x00, 0x00, 0x00, 0x30, 0xFC, 0xBC, 0xFA,
    0x6A, 0xFF, 0x2C, 0xFD, 0x00, 0x00, 0x00, 0x00, // 0x0000FAA0
    0x00, 0x00, 0x00, 0x00, 0x30, 0xFC, 0xBC, 0xFA,
    0x6A, 0xFF, 0x2B, 0xFE, 0x80, 0x00, 0x04, 0x0F, // 0x0000FAB0
    0x00, 0xF7, 0x07, 0xFF, 0x03, 0xFF, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x36, 0x34, 0x4B, 0x20, 0x43, // 0x0000FAC0
    0x50, 0x2F, 0x4D, 0x20, 0x56, 0x65, 0x72, 0x73,
    0x2E, 0x20, 0x32, 0x2E, 0x32, 0x20, 0x28, 0x5A, // 0x0000FAD0
    0x38, 0x30, 0x20, 0x43, 0x42, 0x49, 0x4F, 0x53,
    0x20, 0x56, 0x31, 0x2E, 0x32, 0x20, 0x66, 0x6F, // 0x0000FAE0
    0x72, 0x20, 0x5A, 0x38, 0x30, 0x53, 0x49, 0x4D,
    0x2C, 0x20, 0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, // 0x0000FAF0
    0x67, 0x68, 0x74, 0x20, 0x31, 0x39, 0x38, 0x38,
    0x2D, 0x32, 0x30, 0x30, 0x37, 0x20, 0x62, 0x79, // 0x0000FB00
    0x20, 0x55, 0x64, 0x6F, 0x20, 0x4D, 0x75, 0x6E,
    0x6B, 0x29, 0x0D, 0x0A, 0x00, 0x42, 0x49, 0x4F, // 0x0000FB10
    0x53, 0x3A, 0x20, 0x65, 0x72, 0x72, 0x6F, 0x72,
    0x20, 0x62, 0x6F, 0x6F, 0x74, 0x69, 0x6E, 0x67, // 0x0000FB20
    0x0D, 0x0A, 0x00, 0x7E, 0xB7, 0xC8, 0x4F, 0xCD,
    0xC2, 0xFB, 0x23, 0xC3, 0x33, 0xFB, 0x31, 0x80, // 0x0000FB30
    0x00, 0x21, 0xCB, 0xFA, 0xCD, 0x33, 0xFB, 0xAF,
    0x32, 0x03, 0x00, 0x32, 0x04, 0x00, 0xC3, 0x9B, // 0x0000FB40
    0xFB, 0x31, 0x80, 0x00, 0x0E, 0x00, 0xCD, 0xD9,
    0xFB, 0xCD, 0xD4, 0xFB, 0x06, 0x2C, 0x0E, 0x00, // 0x0000FB50
    0x16, 0x02, 0x21, 0x00, 0xE4, 0xC5, 0xD5, 0xE5,
    0x4A, 0xCD, 0x09, 0xFC, 0xC1, 0xC5, 0xCD, 0x1E, // 0x0000FB60
    0xFC, 0xCD, 0x25, 0xFC, 0xB7, 0xCA, 0x80, 0xFB,
    0x21, 0x1D, 0xFB, 0xCD, 0x33, 0xFB, 0xF3, 0x76, // 0x0000FB70
    0xE1, 0x11, 0x80, 0x00, 0x19, 0xD1, 0xC1, 0x05,
    0xCA, 0x9B, 0xFB, 0x14, 0x7A, 0xFE, 0x1B, 0xDA, // 0x0000FB80
    0x65, 0xFB, 0x16, 0x01, 0x0C, 0xCD, 0x05, 0xFC,
    0xC3, 0x65, 0xFB, 0x3E, 0xC3, 0x32, 0x00, 0x00, // 0x0000FB90
    0x21, 0x03, 0xFA, 0x22, 0x01, 0x00, 0x32, 0x05,
    0x00, 0x21, 0x06, 0xEC, 0x22, 0x06, 0x00, 0x01, // 0x0000FBA0
    0x80, 0x00, 0xCD, 0x1E, 0xFC, 0x3A, 0x04, 0x00,
    0x4F, 0xC3, 0x00, 0xE4, 0xDB, 0x00, 0xC9, 0xDB, // 0x0000FBB0
    0x01, 0xC9, 0x79, 0xD3, 0x01, 0xC9, 0x79, 0xD3,
    0x03, 0xC9, 0xDB, 0x02, 0xC9, 0x79, 0xD3, 0x05, // 0x0000FBC0
    0xC9, 0xDB, 0x05, 0xC9, 0x0E, 0x00, 0xC3, 0x05,
    0xFC, 0x21, 0x00, 0x00, 0x79, 0xFE, 0x04, 0xDA, // 0x0000FBD0
    0xED, 0xFB, 0xFE, 0x08, 0xCA, 0xF9, 0xFB, 0xFE,
    0x09, 0xCA, 0xFF, 0xFB, 0xC9, 0xD3, 0x0A, 0x6F, // 0x0000FBE0
    0x29, 0x29, 0x29, 0x29, 0x11, 0x33, 0xFA, 0x19,
    0xC9, 0x21, 0x9C, 0xFA, 0xC3, 0x02, 0xFC, 0x21, // 0x0000FBF0
    0xAC, 0xFA, 0xD3, 0x0A, 0xC9, 0x79, 0xD3, 0x0B,
    0xC9, 0x79, 0xD3, 0x0C, 0xC9, 0x7A, 0xB3, 0xC2, // 0x0000FC00
    0x18, 0xFC, 0x69, 0x60, 0x2C, 0xC0, 0x24, 0xC9,
    0xEB, 0x09, 0x6E, 0x26, 0x00, 0xC9, 0x79, 0xD3, // 0x0000FC10
    0x0F, 0x78, 0xD3, 0x10, 0xC9, 0xAF, 0xC3, 0x2B,
    0xFC, 0x3E, 0x01, 0xD3, 0x0D, 0xDB, 0x0E, 0xC9, // 0x0000FC20
};

const size_t bios_size = sizeof(bios);

const unsigned char boot[] = {
    0xC3, 0x18, 0x00, 0x42, 0x4F, 0x4F, 0x54, 0x3A,
    0x20, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, // 0x00000000
    0x6F, 0x72, 0x6C, 0x64, 0x21, 0x0D, 0x0A, 0x00,
    0x31, 0x80, 0x00, 0x21, 0x03, 0x00, 0xCD, 0x33, // 0x00000010
    0xFB, 0x76,                                     // 0x00000020
};

const size_t boot_size = sizeof(boot);

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
  printf("RESET-OFF (Delayed 1s)\n");
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

// --- Main Emulation Loop ---
__attribute__((noinline)) void __time_critical_func(emu_loop)(void) {
  PIO pio = pio0;
  uint count = 0;
  uint8_t data_byte = 0;
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
        }
        //        if (ioadrs == 0x00) { // UART TX data
        //          uart_txdata = data_byte;
        //          uart_stat = uart_stat & 0xFD; // b2=0: TX busy
        //        }
        else if (ioadrs == 0x30) { // 0x30 : PPI PA
          // ==== ここから先はSIO直叩き（最速） ====
          if (data_byte & 1) {
            sio_hw->gpio_set = (1u << PA0_PIN); // PA b0 ON (GPIO27)
          } else {
            sio_hw->gpio_clr = (1u << PA0_PIN); // PA b0 OFF (GPIO27)
          }
        }
      } else {                // MREQ=1, WR=1  I/O-Read (not Memory-access)
        if (ioadrs == 0x00) { // === ポート0 : CONSTA ===
          //          data_byte = uart_stat | UART_TX_READY;   //
          //          TXは常に送信可能とみなす
          data_byte = uart_stat;     // 0:not ready, 1:ready
        } else if (ioadrs == 0x01) { // === ポート1 : CONIN ===
          data_byte = uart_rxdata;
          uart_stat = 0;
        }
        //        if (ioadrs == 0x01) { // UART status
        //          data_byte = uart_stat;
        //        } else if (ioadrs == 0x00) { // UART RX data
        //          data_byte = uart_rxdata;
        //          uart_stat &= 0xFE; // RX Data Empty (Clear bit 0)
        //        } else {
        else {
          data_byte = (uint8_t)(agpio >> DATA_BASE);
        }
        pio_sm_put_blocking(pio, sm_emu, data_byte);
      }
    }
    if (false) { // デバッグ用 Z80_freq = 20  (20Hz) で使用する
      printf("%05d MREQ:%d WR:%d RD:%d ADRS:%04X DATA:%02X\n", count,
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

  if (false) { // 高速 コア電圧1.3V クロック 360/400MHz 設定
    sleep_ms(100);
    sysvolt = VREG_VOLTAGE_1_30;
    vreg_set_voltage(sysvolt);
    sleep_ms(100);
    // sysclk = 400000;
    sysclk = 360000;
    if (set_sys_clock_khz(sysclk, true)) {
#if PICO_RP2040
      ssi_hw->baudr = 3; // 400MHz / 3 = 133MHz
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
  memcpy(memory + 0xFA00, bios, sizeof(bios));
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

  printf("GPIO27 初期設定完了（SDK使用）→ 以後SIO直叩きでON/OFF\n");

  sleep_ms(100);

  // Initial CLK pulses (Python: CLK_OnOff(10))
  clk_on_off(10);
  // PIO初期化
  pio_init_bus();

  sleep_ms(2000);
  // EMUZ80_RP2040_PCB
  printf("\n** For EMUZ80_RP2040_PCB! **\n");
  printf("** ROM-DATA: EMUBASIC_IO  **\n");
  printf("\n-hit [Enter] in terminal-\n");
  while (getchar_timeout_us(100) == PICO_ERROR_TIMEOUT)
    ;
  float volt = 0;
  if (sysvolt == VREG_VOLTAGE_1_15)
    volt = 1.15;
  if (sysvolt == VREG_VOLTAGE_1_30)
    volt = 1.30;

  //  エミュレーション開始(core1)
  printf("\nAE-RP2040 Core:%0.2fV Clock:%dMHz\n", volt, sysclk / 1000);
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
  // int Z80_freq = 6000000; // 6MHz
  // int Z80_freq = 4000000; // 4MHz
  int Z80_freq = 2500000; // 2.5MHz
  // int Z80_freq = 20; // 20Hz
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
