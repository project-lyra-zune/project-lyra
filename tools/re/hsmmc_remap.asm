; hsmmc_remap.asm: NvDdkHsmmcSendCommand entry hook that remaps a read
; command's native block address down into the reserved front region of the
; eMMC user area (native blocks 0..441,343, which CE never maps into DSK1:).
;
; ABI: NvDdkHsmmcSendCommand(r0 = hHsmmc, r1 = pCmd). pCmd is a 32-byte
; command struct; pCmd[0x00] = command index, pCmd[0x04] = native (512-byte)
; block address. A DSK1: logical-sector-0 read arrives here as CMD18 (0x12)
; with block address 0x0006BC00 (441,344), i.e. the driver adds a fixed
; 441,344-block base to every logical read.
;
; When armed, and the command is CMD18, and its block address is in the
; [LO, HI) window (the range this session is actively reading), subtract
; DELTA so the read lands at (block - DELTA) instead. That reads the front
; region using the driver's own clocked, DMA'd read path. Reads outside the
; window pass through untouched.
;
; Position independent: internal branches are PC-relative; the four kernel
; scratch-slot addresses and the return VA are absolute immediates from the
; literal pool (fixed every boot, no relocations). The hook overwrites the
; two prologue instructions (PUSH {r4,r5,lr}; MOV r4,r1), which this snippet
; replays before returning to NvDdkHsmmcSendCommand+8.

ARMED   EQU     0x80015400          ; u32: nonzero = remap active
LO      EQU     0x80015404          ; u32: window low bound (inclusive)
HI      EQU     0x80015408          ; u32: window high bound (exclusive)
DELTA   EQU     0x8001540C          ; u32: subtracted from the block address
RETVA   EQU     0xC08E47BC          ; NvDdkHsmmcSendCommand + 8

        AREA    |.text|, CODE, READONLY, ALIGN=2
        EXPORT  |hsmmc_remap|

|hsmmc_remap| PROC
        STMFD   sp!, {r4-r7, lr}
        LDR     r7, =ARMED
        LDR     r6, [r7]
        CMP     r6, #0
        BEQ     done
        LDR     r4, [r1, #0]        ; command index
        CMP     r4, #0x12           ; CMD18 READ_MULTIPLE_BLOCK?
        BNE     done
        LDR     r5, [r1, #4]        ; native block address
        LDR     r7, =LO
        LDR     r4, [r7]
        CMP     r5, r4
        BCC     done                ; block < LO -> pass through
        LDR     r7, =HI
        LDR     r4, [r7]
        CMP     r5, r4
        BCS     done                ; block >= HI -> pass through
        LDR     r7, =DELTA
        LDR     r4, [r7]
        SUB     r5, r5, r4          ; remap into front region
        STR     r5, [r1, #4]
done
        LDMFD   sp!, {r4-r7, lr}
        STMFD   sp!, {r4, r5, lr}   ; replay prologue instruction 1
        MOV     r4, r1              ; replay prologue instruction 2
        LDR     pc, =RETVA
        LTORG
        ENDP
        END
