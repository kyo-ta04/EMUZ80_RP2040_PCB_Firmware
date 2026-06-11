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
// #define WAIT_PIN 27  // GP27: WAIT
#define RESET_PIN 28 // GP28: RESET
#define CLK_PIN 29   // GP29: CLK

#define MEMORY_SIZE 65536 // 64KB
static uint8_t __attribute__((aligned(65536))) memory[MEMORY_SIZE];
volatile bool stop_flg = false;

volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_txdata = 0;
volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_rxdata = 0;
volatile uint8_t __attribute__((section(".scratch_y.uart"))) uart_stat = 1;

// ROM-BASIC (EMUZ80のEMUBASIC)
// @tendai22plusさんによる UART I/Oアクセス改造版
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
  printf("RESET-OFF (Delayed 0.1s)\n");
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
__attribute__((noinline)) void handle_io(uint32_t agpio, volatile uint32_t *txf) {
// static void handle_io(uint32_t agpio, volatile uint32_t *txf) {
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

#ifndef USE_EMU_LOOP_ASM
#define USE_EMU_LOOP_ASM 1
#endif

#if USE_EMU_LOOP_ASM
// ============================================================
// 【ASM版 emu_loop() Z80バス・エミュレーション】 
// - トップレベル asm(...) による完全記述版
//   ファイル分割なし / #if USE_EMU_LOOP_ASM で切替
//
// この版は C版を -O3 でコンパイルした実際の elf の逆アセンブル結果を
// ベースに、命令・分岐・構造を極力忠実に再現した手書きアセンブリです。
//
// 重要なポイント:
//   - .section .time_critical.emu_loop を明示（C版の __time_critical_func と同等）
//   - naked ではなくトップレベル asm で提供（属性の無視や配置のずれを回避）
//   - ポーリングは C版と同じタイトな bne 構造
//   - I/O パスも C版と同じシンプルな stop / bl / resume
//
// これで C版と「本体命令 + 配置」の両面で最も近い状態になります。
//
// 【最適化のポイント (C版と共通)】
//   * ループに入る前に「頻繁に使う値」をレジスタへ一括キャッシュ
//       r9  = memory ベース (64KB Z80 RAM)
//       r1  = &pio0->txf[sm_emu]   (Z80 へのデータ出力 FIFO)
//       r6  = &pio0->rxf[sm_emu]   (Z80 からの (addr|data|ctrl) 入力 FIFO)
//       r4  = RXEMPTY ビットマスク (1<<(PIO_FSTAT_RXEMPTY_LSB + sm))
//       sl  = &clk_pwm_slice_num (I/O時のみ使用)
//       r7/r8 = PWM 原子操作用 SET/CLR エイリアスベース (0x400aa000 / 0x400ab000)
//   * Memory Read を「一番最初の条件分岐」にして最速直線パス化
//       (Z80 実機で最も多いバスサイクルが MemRead であるため)
//   * while (fstat & mask) { tight } のポーリングを 2命令ループに
//   * I/O アクセスのみ「低頻度」として分岐。クロック停止に直接 PWM レジスタ
//     原子ストア (str to alias) を使い、C 関数呼び出しは handle_io のみ。
//   * handle_io 呼び出し前後で r0=agpio, r1=txf を保持したまま bl するため
//     余計な mov は不要 (コンパイラが自動的にそうしていたのを踏襲)
//
// 【レジスタマップ (ループ内主なもの)】
//   r0  : agpio (受信した 32bit: [31:24]制御 | [23:16]data | [15:0]addr )
//   r2  : アドレス (uxth で下16bit抽出)
//   ip  : ライトデータ一時 (mov.w ip, r0, lsr #16)
//   r3  : 作業レジスタ (bus_state 抽出結果、fstat 値など)
//   fp  : PWM イネーブルビット (I/O path のみ)
//
// 【注意】
//   - この関数は Core1 で永遠に回る __time_critical ホットパス。
//   - 停止は Core0 の task1 側で Ctrl-D により別ルートで扱う (この中では stop_flg を見ない)
//   - 切替はビルド時に USE_EMU_LOOP_ASM=0/1 で。ASM=1 の時は下の asm() が実体を提供。
// ============================================================

// --- Main Emulation Loop ---
// プロトタイプ宣言（C 側から multicore_launch_core1(emu_loop) を呼ぶために必要。
// ASM版使用時もこの宣言でコンパイラは満足し、実際の定義は下の asm() が提供する）
void emu_loop(void);
asm(
  ".syntax unified\n"                        // 統一アセンブリ構文 (Thumb-2 命令も使用可能)
  ".thumb\n"                                 // Thumb モードでアセンブル
  ".section .time_critical.emu_loop,\"ax\",%progbits\n"  // C版 __time_critical_func と同等のセクションに配置（copy_to_ram 対象）
  ".balign 4\n"                              // 4バイト境界にアライン（命令フェッチ効率化）
  ".thumb_func\n"                            // Thumb 関数であることをリンカに通知
  ".global emu_loop\n"                       // 外部（main や multicore）から参照可能にする
  ".type   emu_loop, %function\n"            // シンボルを関数として定義（サイズ情報なども付与）
"emu_loop:\n"                                // emu_loop 関数のエントリポイント（ここから実行開始）
  "    stmdb   sp!, {r3, r4, r5, r6, r7, r8, r9, sl, fp, lr}\n"  // C ABI に従い使用レジスタを退避 (r3 も含むのはコンパイラ生成に合わせた)
  "    movs    r4, #1\n"                                          // r4 = 1 (後でマスク作成用)
  "    ldr     r3, =sm_emu\n"              // sm_emu (現在は1固定) のアドレス取得
  "    ldr     r1, =0x50200010\n"          // PIO0 TX FIFO ベース (sm調整前)。txf[0] = 0x50200010
  "    ldr     r3, [r3]\n"                 // r3 = sm_emu の値 (1)
  "    ldr.w   r9, =memory\n"              // r9 = Z80 エミュレーション用 64KB メモリ先頭 (&memory[0])
  "    lsls    r6, r3, #2\n"               // r6 = sm * 4  (バイトオフセット計算用)
  "    add     r1, r6\n"                   // r1 = &txf[sm]   ここが Z80 への応答データ書込み先
  "    adds    r3, #8\n"                   // r3 = sm + 8    → fstat の RXEMPTY ビット位置 (LSB=8 + sm)
  "    add.w   r6, r6, #0x50000000\n"      // r6 に PIO0 ベース (0x50000000) を加算（後の +0x200020 で rxf[sm] ベース 0x50200020 + sm*4 になる）
  "    ldr.w   sl, =clk_pwm_slice_num\n"   // I/O パスで「今どの PWM スライスで CLK を出しているか」を得るためのポインタ
  "    ldr     r5, =0x50200000\n"          // r5 = PIO0 レジスタベース (fstat は +4)
  "    ldr.w   r8, =0x400ab000\n"          // r8 = PWM ペリフェラル CLR エイリアスベース (原子クリア)
  "    ldr     r7, =0x400aa000\n"          // r7 = PWM ペリフェラル SET エイリアスベース (原子セット)
  "    add.w   r6, r6, #0x200020\n"        // r6 = 0x50200020 + (sm*4)  → &rxf[sm]  (Z80から来る addr+data+busstate の受信元)
  "    lsls    r4, r3\n"                   // r4 = 1 << (sm+8)   これが RXEMPTY 検出マスクになる

  // ========================================================
  //  メインループ (超ホットパス)
  //   - PIO RX FIFO が空なら fstat ポーリングで待つ
  //   - 1 回受信したら即アドレス・バス状態をデコード
  //   - C版 -O3 出力と同一のタイトな bne 構造を再現
  // ========================================================
"1:\n"                                   // poll_loop (ローカルラベル)
  "    ldr     r3, [r5, #4]\n"             // r3 = pio0->fstat
  "    tst     r3, r4\n"                   // RXEMPTY ビットが立っているか？
  "    bne.n   1b\n"                       // 立っていればまだデータなし → 即ループ (2命令の超軽量ポーリング)

  "    ldr     r0, [r6]\n"                 // agpio = *rxf[sm] (32bit一括) [15:0]=A0-15, [23:16]=D0-7(ライト時有効),[26]=WR#,[24]=MREQ#
  "    and.w   r3, r0, #0x05000000\n"      // bus_state = agpio & 0x05000000 (MREQ|WR の 2bit のみ抽出)
  "    cmp.w   r3, #0x04000000\n"          // 0x04000000 == (MREQ=0, WR=1) → Memory Read
  "    uxth    r2, r0\n"                   // r2 = アドレス (下位16bitをゼロ拡張で取り出し)
  "    bne.n   2f\n"                       // Read でなければ分岐

  // ---- Memory Read (最もホットな直線パス) ----
  "    ldrb.w  r3, [r9, r2]\n"             // r3 = memory[address]   8bit ロード
  "    str     r3, [r1]\n"                 // *txf[sm] = data        PIO経由で Z80 データバスへ出力
  "    b.n     1b\n"                       // メモリリード完了 → 即座に poll ループへ戻る（最速直線パスを維持）

"2:\n"
  "    mov.w   ip, r0, lsr #16\n"          // ip = (agpio >> 16) & 0xff   ライトデータを取り出し
  "    cbnz    r3, 3f\n"                   // bus_state 抽出結果(r3)が 0 でなければ I/O アクセス

  // ---- Memory Write ----
  "    strb.w  ip, [r9, r2]\n"             // memory[address] = data
  "    b.n     1b\n"

"3:\n" // ---- I/O Read/Write ----
       //   ここに来るのは低頻度。クロックを止めて C の handle_io を呼ぶ
  "    mov.w   fp, #1\n"                   // PWM 直接操作を再現(インライン展開された clk_pwm_output_off/on)
  "    ldr.w   r3, [sl]\n"                 // r3 = clk_pwm_slice_num の現在値
  "    lsl.w   fp, fp, r3\n"               // fp = (1 << slice_num)
  "    str.w   fp, [r8, #0xf0]\n"          // PWM->en の CLR エイリアスへ書込み → 指定ビットだけ即時クリア (CLK 停止)
  
  "    bl      handle_io\n"                // handle_io(agpio in r0, txf in r1) 呼び出し規約により r0/r1を引数として渡せる
   
  "    str.w   fp, [r7, #0xf0]\n"          // PWM->en の SET エイリアスへ書込み → ビットセット (CLK 再開)
  "    b.n     1b\n"

  ".size emu_loop, . - emu_loop\n"
  ".ltorg\n"
);

#else
// ============================================================
// オリジナル C 版 emu_loop() Z80バス・エミュレーション
// ============================================================
void __time_critical_func(emu_loop)(void) {
  // PIO レジスタ・マスク・ポインタをキャッシュ（ループ外で1回だけ）
  uint8_t *const mem_ptr = memory;
  const uint32_t rxempty_mask = (1u << (PIO_FSTAT_RXEMPTY_LSB + sm_emu));
  // PIO FIFO
  volatile uint32_t *const rxf = (volatile uint32_t *)&pio0_hw->rxf[sm_emu];
  volatile uint32_t *const txf = (volatile uint32_t *)&pio0_hw->txf[sm_emu];

  // Z80 MREQ(24) と WR(26) のビットだけを抽出するマスク
  const uint32_t bus_mask      = (1u << MREQ_PIN) | (1u << WR_PIN);
  const uint32_t mem_read_state  = (1u << WR_PIN); // MREQ=0, WR=1
  const uint32_t mem_write_state = 0u;              // MREQ=0, WR=0

  while (true) {
    // PIOのRX FIFOからバスの状態を取得する(Z80 RD又はWR有効時、ブロッキング)
    while (pio0_hw->fstat & rxempty_mask) {
      tight_loop_contents();
    }
    uint32_t agpio = *rxf;      // agpio = address[15:0], data[7:0], MREQ[24],WR[26]
    uint32_t adrs_word = (agpio & 0xFFFF);
    uint32_t bus_state = agpio & bus_mask;

    // Z80 Memory-Read を最速の直線パスにする (最頻出)
    if (__builtin_expect(bus_state == mem_read_state, 1)) {
      *txf = mem_ptr[adrs_word]; // Z80 Memory-Read
    } else if (bus_state == mem_write_state) {
      mem_ptr[adrs_word] = (uint8_t)(agpio >> DATA_BASE); // Z80 Memory-Write
    } else {   // MREQ=1 - Z80 I/Oアクセス
      clk_pwm_output_off();   // Z80クロック停止 (inline)
      handle_io(agpio, txf);  // Z80 I/Oアクセス処理
      clk_pwm_output_on();    // Z80クロック再開 (inline)
    }
  }
}
#endif

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
  #if USE_EMU_LOOP_ASM
  printf("\nWaveshare RP2350-Zero v1.1-ASM Core:%0.2fV Clock:%dMHz\n", volt,
         sysclk / 1000);
  #else
  printf("\nWaveshare RP2350-Zero v1.1-C Core:%0.2fV Clock:%dMHz\n", volt,
         sysclk / 1000);
  #endif
  printf("Emulation task(core1) Start..\n");
  multicore_launch_core1(emu_loop);
  sleep_ms(0);

  // CLK PWM Setup, RP2350 400MHz Z80 17MHz, 360MHz Z80 12MHz, 150MHz Z80 6MHz
  // int Z80_freq = 18000000; // 17MHz
  // int Z80_freq = 17000000; // 17MHz
  // int Z80_freq = 16000000; // 16MHz
  // int Z80_freq = 15000000; // 15MHz
  // int Z80_freq = 14000000; // 14MHz
  // int Z80_freq = 12000000; // 12MHz
  // int Z80_freq = 11000000; // 11MHz
  // int Z80_freq = 10000000; // 10MHz
  // int Z80_freq = 9000000; // 9MHz
  // int Z80_freq = 8000000; // 8MHz
  // int Z80_freq = 7000000; // 7MHz
  int Z80_freq = 6000000; // 6MHz
  // int Z80_freq = 4000000; // 4MHz
  // int Z80_freq = 2500000; // 2.5MHz
  // int Z80_freq = 20; // 20Hz

  // gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);
  //  uint slice_num = pwm_gpio_to_slice_num(CLK_PIN);
  //  set_pwm_freq(CLK_PIN, Z80_freq);
  init_clk_pwm(Z80_freq);
  // pwm_set_enabled(slice_num, true);
  printf("Z80 CLK-ON %fMHz\n", Z80_freq / 1000000.0);

  // 1秒後にRESETを解除
  add_alarm_in_ms(100, reset_off_callback, NULL, false);

  printf("main task1(Core0) start..\n");
  task1();

  // Cleanup
  gpio_put(RESET_PIN, 0);
  printf("RESET-ON\n");
  sleep_ms(100);
  // pwm_set_enabled(slice_num, false);
  clk_pwm_output_off();
  clk_on_off(10);
  printf("Exited.\n");

  return 0;
}
