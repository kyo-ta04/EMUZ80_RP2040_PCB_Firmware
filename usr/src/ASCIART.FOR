0 VARIABLE F
0 VARIABLE Y
0 VARIABLE X
0 VARIABLE C
0 VARIABLE D
0 VARIABLE A
0 VARIABLE B
0 VARIABLE II
0 VARIABLE Q
0 VARIABLE S
0 VARIABLE T
0 VARIABLE P
: RUN
  CR
  50 F !
  25 0 DO I 12 - Y !
    79 0 DO I 39 - X !
      X @ 229 * 100 / C !
      Y @ 416 * 100 / D !
      C @ A !
      D @ B !
      17 0 DO
        I II !
        B @ F @ / Q !
        B @ Q @ F @ * - S !
        A @ A @ * B @ B @ * - F @ / C @ + T !
        2 A @ Q @ * A @ S @ * F @ / + * D @ + B !
        T @ A !
        A @ F @ / P !
        B @ F @ / Q !
        P @ P @ * Q @ Q @ * + 4 > IF
          LEAVE
        THEN
      LOOP
      II @ 15 > IF
        SPACE
      ELSE
        II @ DUP
        9 > IF 7 + THEN
        48 + EMIT
      THEN
    LOOP
    CR
  LOOP
  ." OK" CR FIRST 80 EXPECT CR
;

