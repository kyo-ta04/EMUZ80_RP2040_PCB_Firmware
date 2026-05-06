/*
 * Derived from Version 7 Unix cat.c
 * Source: https://github.com/v7unix/v7unix/blob/master/v7/usr/src/cmd/cat.c
 *
 * Copyright 2026 DragonBallEZ(kyo-ta04)
 * Licensed under the MIT License for this repository and my modifications.
 * Original V7 Unix-derived code is provided with attribution.
 */

#include <stdio.h>

char	stdbuf[BUFSIZ];

int main(int argc, char **argv)
{
	int fflg = 0;
	register FILE *fi;
	register int c;

	setbuf(stdout, stdbuf);
	for( ; argc>1 && argv[1][0]=='-'; argc--,argv++) {
		switch(argv[1][1]) {
		case 0:
			break;
		case 'u':
			setbuf(stdout, (char *)NULL);
			continue;
		}
		break;
	}
	if (argc < 2) {
		argc = 2;
		fflg++;
	}
	while (--argc > 0) {
		if (fflg || (*++argv)[0]=='-' && (*argv)[1]=='\0')
			fi = stdin;
		else {
			if ((fi = fopen(*argv, "r")) == NULL) {
				fprintf(stderr, "cat: can't open %s\n", *argv);
				continue;
			}
		}
		while ((c = getc(fi)) != EOF)
			putchar(c);
		if (fi!=stdin)
			fclose(fi);
	}
	return(0);
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
