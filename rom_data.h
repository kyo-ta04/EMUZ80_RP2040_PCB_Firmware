#ifndef ROM_DATA_H
#define ROM_DATA_H

#include <stddef.h>
#include <stdint.h>

#define ROMDISK_SIZE (256 * 1024)

// CCP + BDOS
extern const uint8_t ccp_bdos[];
extern const size_t ccp_bdos_size;

// BIOS
extern const uint8_t bios01[];
extern const size_t bios01_size;

// ROM Disks (256KB each)
extern const uint8_t romdisk[];       // DISK0: cpm22-1.dsk
extern const uint8_t cpm22_disk1[];   // DISK1: cpm22_disk1.dsk
extern const uint8_t tp301a[];        // DISK2: cpm22_tp301a.dsk
extern const uint8_t z80forth[];      // DISK3: cpm22_z80forth.dsk
extern const uint8_t cpm22_htc[];     // DISK4: cpm22_htc.dsk (stub)

#endif // ROM_DATA_H
