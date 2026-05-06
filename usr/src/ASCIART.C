/*
cpm c asciart.c -LF
*/

#include <stdio.h>

char	junk[256];

main()
{
	int i, x, y;
	float a, b, ca, cb, t;

	printf("hit Enter key:");
	gets(junk);

	for (y = -12; y <= 12; y++) {
		for (x = -39; x <= 39; x++) {
			ca = x * 0.0458;
			cb = y * 0.08333;
			a = ca;
			b = cb;
			for (i = 0; i <=15; i++) {
				t = a * a - b * b + ca;
				b = 2 * a * b + cb;
				a = t;
				if ((a * a + b * b) > 4) {
					break;
				}
			}
			putch("0123456789ABCDEF "[i]);
		}
		putch('\n');
	}
	printf("OK\n");
	printf("hit Enter key:\n");
	gets(junk);
}

