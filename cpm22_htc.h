/* Stub for future "cpm22_htc.dsk" disk image
 * Generated placeholder — replace with bin2c output when ready.
 * サイズ: 256256 バイト (= 128 * 77 * 26)
 * アドレスオフセット: 0x0
 */

#include <pico.h>
#include <stddef.h>
#include <stdint.h>

#define ROM_SIZE_I (325 * 2048) // 650KB (665,600B) (325*2048) (128 * 26 * 200)
const uint8_t __in_flash() __attribute__((aligned(4))) cpm22_htc[ROM_SIZE_I] = {
  [0 ... ROM_SIZE_I - 1] = 0xE5 // E5で埋めて未使用ディスクとする
};
