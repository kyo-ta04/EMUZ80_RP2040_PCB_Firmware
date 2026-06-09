// EMUZ80_RP2040_PCB_Firmware: Z80 bus emulator for Waveshare RP2350-Zero
// ** For EMUZ80_RP2040_PCB! **
// ** ROM-DATA: EMUBASIC_IO  **

// #include "AE-RP2040.pio.h"
#include "RP2350-Zero.pio.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#endif
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
#define MREQ_PIN 24  // GP24: MREQ
#define RD_PIN 25    // GP25: RD
#define WR_PIN 26    // GP26: WR
#define WAIT_PIN 27  // GP27: WAIT
#define RESET_PIN 28 // GP28: RESET
#define CLK_PIN 29   // GP29: CLK

// // #define MEMORY_SIZE 2048
// #define MEMORY_SIZE 65536 // 64KB
// static uint8_t memory[MEMORY_SIZE];

// Z80用メモリー
#define MEMORY_SIZE 65536 // 64KB
static uint8_t __attribute__((aligned(4))) memory[MEMORY_SIZE] = {
    [0 ... MEMORY_SIZE - 1] = 0xFF};
volatile bool stop_flg = false;


// volatile uint8_t uart_txdata = 0;
// volatile uint8_t uart_rxdata = 0;
// volatile uint8_t uart_stat = 1;

volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_txdata = 0;
volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_rxdata = 0;
volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_stat = 1;



// Test Program (from Python testprg2)
const uint8_t testprg2[] = {0x21, 0x00, 0x00,  // LD HL, 0000
                            0x22, 0x00, 0x80,  // LD (8000), HL
                            0x23,              // INC HL
                            0xC3, 0x03, 0x00}; // JP 0003

// ROM-BASIC (EMUZ80のEMUBASIC)
// @tendai22plusさんによる UART I/Oアクセス改造版
#define EMUBASIC_IO
#include "emubasic_io.h"

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
  hw_set_bits(&pwm_hw->en, 1u << clk_pwm_slice_num);
}

static inline void clk_pwm_output_off(void) {
  hw_clear_bits(&pwm_hw->en, 1u << clk_pwm_slice_num);
}

static void init_clk_pwm(uint32_t freq_hz) {
  clk_pwm_init();
  clk_pwm_set_frequency(freq_hz);
  clk_pwm_output_on();
}



// QSPIクロックを調整する関数
void set_qspi_clock_divider(uint32_t sys_clock_khz, uint32_t qspi_max_khz) {
  uint32_t divider = (sys_clock_khz + qspi_max_khz - 1) / qspi_max_khz;
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                  sys_clock_khz * 1000, sys_clock_khz * 1000 / divider);
}


// PIO0
uint sm_emu = 1;
uint sm_dirsL = 2;
uint sm_dirsH = 3;

// PIO1
// uint sm_trg2 = 0;
uint sm_trg_RD = 0;
uint sm_trg_WR = 1;

// --- PIO Helpers ---
void pio_init_bus() {
  PIO pio = pio0;
  PIO pio_1 = pio1;

  // PIO1 - SM0/1: trg_pin (Detection of falling edge on RD/WR)
  uint offset_trg = pio_add_program(pio_1, &trg_pin_program);
  pio_sm_config c_trg_RD = trg_pin_program_get_default_config(offset_trg);
  pio_sm_config c_trg_WR = trg_pin_program_get_default_config(offset_trg);

  // PIO0 - SM0: m_emu (Address/Data handling)
  uint offset_emu = pio_add_program(pio, &m_emu_program);
  pio_sm_config c_emu = m_emu_program_get_default_config(offset_emu);

  // PIO0 - SM1/2: d_pindirs (Direction toggle)
  uint offset_dirs = pio_add_program(pio, &d_pindirs_program);
  pio_sm_config c_dirsL = d_pindirs_program_get_default_config(offset_dirs);
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

  // PIO1 SM0/1: trg_pin (Detection of falling edge on RD/WR)
  sm_config_set_in_pins(&c_trg_RD, RD_PIN); // base = GP25 (RD)
  pio_sm_init(pio_1, sm_trg_RD, offset_trg, &c_trg_RD);
  pio_sm_set_enabled(pio_1, sm_trg_RD, true);

  sm_config_set_in_pins(&c_trg_WR, WR_PIN); // base = GP26 (WR)
  pio_sm_init(pio_1, sm_trg_WR, offset_trg, &c_trg_WR);
  pio_sm_set_enabled(pio_1, sm_trg_WR, true);

  // PIO0 - SM0: m_emu (Address/Data handling)
  sm_config_set_in_pins(&c_emu, 0);             // base = GP0
  sm_config_set_out_pins(&c_emu, DATA_BASE, 8); // base = GP16, cnt = 8
  sm_config_set_jmp_pin(&c_emu, RD_PIN);        // GP25 (RD)

  // D0-7ピン初期化(入力)
  pio_sm_set_consecutive_pindirs(pio, sm_emu, DATA_BASE, 8, false);
  // シフトレジスタの設定 (Auto Push/Pull 有効化)
  // ISRのシフト方向, auto_push=true, threshold=30
  sm_config_set_in_shift(&c_emu, false, true, 30);
  // OSRのシフト方向, auto_pull=true, threshold=8
  sm_config_set_out_shift(&c_emu, true, true, 8);

  pio_sm_init(pio, sm_emu, offset_emu, &c_emu);
  pio_sm_set_enabled(pio, sm_emu, true);

  // PIO0 - SM1/2: d_pindirs (Direction toggle)
  sm_config_set_set_pins(&c_dirsL, DATA_BASE, 4); // GP16..19
  sm_config_set_jmp_pin(&c_dirsL, RD_PIN);        // GP25 (RD)
  pio_sm_init(pio, sm_dirsL, offset_dirs, &c_dirsL);
  pio_sm_set_enabled(pio, sm_dirsL, true);

  sm_config_set_set_pins(&c_dirsH, DATA_BASE + 4, 4); // GP20..23
  sm_config_set_jmp_pin(&c_dirsH, RD_PIN);            // GP25 (RD)
  pio_sm_init(pio, sm_dirsH, offset_dirs, &c_dirsH);
  pio_sm_set_enabled(pio, sm_dirsH, true);
}

// --- UART Task (Core 0) ---
void task1(void) {
  printf("task1 UART start..\n");
  while (true) {
    // 送信処理 (Z80 -> USB)
    if ((uart_stat & 0x02) == 0) {
      if (tud_cdc_connected() && tud_cdc_write_available() > 0) {
        putchar(uart_txdata);
        uart_stat |= 0x02; // TX Buffer Empty
      }
    }

    // 受信処理(US->Z80) RX Readyが0(空)の場合のみ入力をチェック
    if (!(uart_stat & 0x01)) {
      int c = getchar_timeout_us(0);
      if (c != PICO_ERROR_TIMEOUT) {
        if (c == 0x04) { // Ctrl-D: Stop emulation
          printf("\ntask1: Ctrl-D detected. Stopping..\n");
          stop_flg = true;
          break;
        }
        uart_rxdata = (uint8_t)c;
        uart_stat |= 0x01; // RX Data Available
        // printf("[%c]",c);
      }
    }
    sleep_ms(1);
  }
}

// --- I/O Handler (noinline: emu_loop のホットパスからレジスタ圧迫を排除) ---
__attribute__((noinline))
static void handle_io(uint32_t agpio, volatile uint32_t *txf) {
  const uint32_t wr_mask = (1u << WR_PIN);
  if (!(agpio & wr_mask)) { // MREQ=1, WR=0  I/O-Write (not Memory-access)
    if ((uint8_t)agpio == 0x00) { // UART TX data
      uart_txdata = (uint8_t)(agpio >> DATA_BASE);
      uart_stat = uart_stat & 0xFD; // b2=0: TX busy
    }
  } else {                // MREQ=1, WR=1  I/O-Read (not Memory-access)
    if ((uint8_t)agpio == 0x01) { // UART status
      *txf = uart_stat;
    } else if ((uint8_t)agpio == 0x00) { // UART RX data
      *txf = uart_rxdata;
      uart_stat &= 0xFE; // RX Data Empty (Clear bit 0)
    } else {
      *txf = (uint8_t)(agpio >> DATA_BASE);
    }
  }
}

// --- Main Emulation Loop ---
void __time_critical_func(emu_loop)(void) {
  // PIO レジスタ・マスク・ポインタをキャッシュ（ループ外で1回だけ）
  uint8_t *const mem_ptr = memory;
  const uint32_t rxempty_mask = (1u << (PIO_FSTAT_RXEMPTY_LSB + sm_emu));

  volatile uint32_t *const rxf = (volatile uint32_t *)&pio0_hw->rxf[sm_emu];
  volatile uint32_t *const txf = (volatile uint32_t *)&pio0_hw->txf[sm_emu];

  // MREQ(24) と WR(26) のビットだけを抽出するマスク
  const uint32_t bus_mask      = (1u << MREQ_PIN) | (1u << WR_PIN);
  const uint32_t mem_read_state  = (1u << WR_PIN); // MREQ=0, WR=1
  const uint32_t mem_write_state = 0u;              // MREQ=0, WR=0

  while (true) {
    // PIOのRX FIFOからデータを取得する(ブロッキング)
    while (pio0_hw->fstat & rxempty_mask) {
      tight_loop_contents();
    }
    uint32_t agpio = *rxf;
    uint32_t adrs_word = (agpio & 0xFFFF);
    uint32_t bus_state = agpio & bus_mask;

    // Memory-Read を最速の直線パスにする (最頻出)
    if (__builtin_expect(bus_state == mem_read_state, 1)) {
      *txf = mem_ptr[adrs_word]; // Memory-Read
    } else if (bus_state == mem_write_state) {
      mem_ptr[adrs_word] = (uint8_t)(agpio >> DATA_BASE); // Memory-Write
    } else {   // MREQ=1  I/Oアクセス
      clk_pwm_output_off();   // Z80クロック停止 (inline)
      handle_io(agpio, txf);
      clk_pwm_output_on();    // Z80クロック再開 (inline)
    }
  }
}

//
//  メイン関数
//
int main() {
  sleep_ms(0);
  uint32_t sysclk = clock_get_hz(clk_sys);
  int sysvolt = VREG_VOLTAGE_1_15;

  if (true) { // 高速 コア電圧1.3V クロック 360/400MHz 設定
    sleep_ms(0);
    sysvolt = VREG_VOLTAGE_1_30;
    vreg_set_voltage(sysvolt);
    sleep_ms(100);
    sysclk = 400000;
    // sysclk = 360000;
    set_sys_clock_khz(sysclk, true);
    set_qspi_clock_divider(sysclk, 133000); // QSPIクロックを133MHz以下に
    sleep_ms(0);
  }
  stdio_init_all();
  setbuf(stdout, NULL); // 標準出力のバッファリングを無効化
  sleep_ms(0);

  // Z80用メモリー初期化
  memset(memory, 0xFF, MEMORY_SIZE);
  memcpy(memory, emuz80_binary, sizeof(emuz80_binary));

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

  sleep_ms(0);

  // Initial CLK pulses (Python: CLK_OnOff(10))
  clk_on_off(10);
  // PIO初期化
  pio_init_bus();

  sleep_ms(800);
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
  printf("\nWaveshare RP2350-Zero Core:%0.2fV Clock:%dMHz\n", volt,
         sysclk / 1000);
  printf("Emulation task(core1) Start..\n");
  multicore_launch_core1(emu_loop);
  sleep_ms(100);

  // CLK PWM Setup, RP2350 400MHz Z80 16MHz, 360MHz Z80 12MHz, 150MHz Z80 6MHz
  // int Z80_freq = 18000000; // 17MHz
   int Z80_freq = 17000000; // 17MHz
  // int Z80_freq = 16000000; // 16MHz
  // int Z80_freq = 15000000; // 15MHz
  // int Z80_freq = 14000000; // 14MHz
  // int Z80_freq = 12000000; // 12MHz
  // int Z80_freq = 11000000; // 11MHz
  // int Z80_freq = 10000000; // 10MHz
  // int Z80_freq = 9000000; // 9MHz
  // int Z80_freq = 8000000; // 8MHz
  // int Z80_freq = 7000000; // 7MHz
  // int Z80_freq = 6000000; // 6MHz
  // int Z80_freq = 4000000; // 4MHz
  // int Z80_freq = 2500000; // 2.5MHz
  // int Z80_freq = 20; // 20Hz
  gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);
  uint slice_num = pwm_gpio_to_slice_num(CLK_PIN);
//  set_pwm_freq(CLK_PIN, Z80_freq);
  init_clk_pwm(Z80_freq);
  // pwm_set_enabled(slice_num, true);
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
