/*
	hd - hex dump

  Copyright (c) 2026 @DragonBallEZ(kyo-ta04)
  SPDX-License-Identifier: MIT
*/

#include	<stdio.h>
#include	<ctype.h>
#include	<stdlib.h>

typedef unsigned short uint16_t;

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


int	main(int argc, char *argv[]) {
  int i;
  FILE	*fin = NULL;
  uint16_t adrs = 0x100;
  int c;
  unsigned char cbuf[16+1];

    /* Parse command line arguments */
	for( ; argc>1 && argv[1][0]=='-'; argc--,argv++) {
		if (argv[1][1] == 'A' || argv[1][1] == 'a') {
      adrs = xtoi(&argv[1][2]);
      continue;
    }
		break;
	}
  if (argc >= 2) {
    fin = fopen(argv[1], "rb");
    if (fin == NULL) {
      fprintf(stderr,"can't open : %s\n", argv[1]);
      return -1;
    }
  } else {
    fin = stdin;
  }

  for (i = 0 ;;) {
    if ((c = getc(fin)) == EOF)
      break;
    if (i == 0) {
      puthex((adrs >> 8) & 0xFF);
      puthex(adrs & 0xFF); 
    }
    putchar(' ');
    puthex(c);
    if (c < ' ' || c > 126) {
      c = '.';
    }
    cbuf[i] = c;
    adrs++;
    if (++i == 16) {
      cbuf[i] = '\0';    /* EOS */
      printf("  %s\n", cbuf);
      i = 0;
    }
  }
  if (i > 0) {
    cbuf[i] = '\0';    /* EOS */
    printf("  %s\n", cbuf);
  }
  fclose(fin);
  return 0;
}

