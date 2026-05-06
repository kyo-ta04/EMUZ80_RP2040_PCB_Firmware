/*
 * ihex - Binary to Intel HEX Converter
 *
 * This utility converts binary files (e.g., CP/M .COM files) into 
 * Intel HEX format. Default start address is 0x0100.
 *
 * Copyright (c) 2026 @DragonBallEZ (kyo-ta04)
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

/* Maximum data length per HEX record */
#define RECORD_LEN 16

/* Simple hex string to integer converter */
unsigned int xtoi(char *s) {
    unsigned int val = 0;
    while (*s) {
        unsigned char c = *s++;
        val <<= 4;
        if (c >= '0' && c <= '9') val += c - '0';
        else if (c >= 'A' && c <= 'F') val += c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') val += c - 'a' + 10;
    }
    return val;
}

void puthex(unsigned char c) {
  static char hex[] = "0123456789ABCDEF";
  unsigned char l = c & 0x0F;
  unsigned char h = (c >> 4) & 0x0F;
  putchar(hex[h]);
  putchar(hex[l]);
}


void main(int argc, char **argv) {
    FILE *fp;
    unsigned char buf[RECORD_LEN];
    unsigned int addr = 0x100;
    int n, i;
    unsigned char checksum;
    char *target_file = NULL;

    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* Option -aXXXX to set start address */
            if (argv[i][1] == 'a' || argv[i][1] == 'A') {
                addr = xtoi(&argv[i][2]);
            }
        } else {
            target_file = argv[i];
        }
    }

    if (target_file == NULL) {
        printf("Usage: ihex [-aAddress] <filename>\n");
        printf("Example: ihex -a0000 PROGRAM.BIN\n");
        return;
    }

    /* Open file in binary mode for CP/M */
    if ((fp = fopen(target_file, "rb")) == NULL) {
        printf("Error: Cannot open %s\n", target_file);
        return;
    }

    /* Read binary data and output Intel HEX format */
    while ((n = fread(buf, 1, RECORD_LEN, fp)) > 0) {
        putchar(':');

        /* 1. Record Length */
        puthex(n);
        checksum = (unsigned char)n;

        /* 2. Load Address (16-bit) */
        puthex((unsigned char)(addr >> 8));
        puthex((unsigned char)(addr & 0xFF));
        checksum += (unsigned char)(addr >> 8);
        checksum += (unsigned char)(addr & 0xFF);

        /* 3. Record Type (00 = Data) */
        printf("00");
        checksum += 0x00;

        /* 4. Data bytes and checksum calculation */
        for (i = 0; i < n; i++) {
            puthex(buf[i]);
            checksum += buf[i];
        }

        /* 5. Checksum (2's complement of total sum) */
        puthex((unsigned char)(0x100 - checksum));
        putchar('\n');
        addr += n;
    }

    /* End of File Record */
    printf(":00000001FF\n");

    fclose(fp);
}

#if CPM
extern  char ** _getargs();
extern  int     _argc_;

int junk() {
  char **argv;
  int argc;

  if(argc == 1) {
    argv = _getargs(0, "argtest");
    argc = _argc_;
  }
}
#endif
