        ORG 0000h
	.include "io_asm.inc"

        DI
        LD SP,9000h

        LD A, 'H'
        OUT (CONDAT),A
        LD A, 'i'
        OUT (CONDAT),A
        LD A, 0Ah
        OUT (CONDAT),A
        LD A, 0Dh
        OUT (CONDAT),A

        LD HL,8045h
        LD (HL),055h
        LD A,(HL)
        CALL HEXOUT

        LD (HL),0AAh
        LD A,(HL)
        CALL HEXOUT

loop:
        JP loop

HEXOUT:
        PUSH AF
        RRCA
        RRCA
        RRCA
        RRCA
        CALL NIBBLE
        POP AF
        CALL NIBBLE
        LD A,13
        OUT (CONDAT),A
        LD A,10
        OUT (CONDAT),A
        RET

NIBBLE:
        AND 0Fh
        ADD A,'0'
        CP '9'+1
        JP C,OUTC
        ADD A,7
OUTC:
        OUT (CONDAT),A
        RET
