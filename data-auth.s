.text
.globl _main
.p2align 2
_main:
    sub sp, sp, #16
    mov w8, #42
    str w8, [sp]
    mov x16, sp
    mov x10, #13
    pacda x16, x10
    cmp w0, #1
    b.ne Lbad
    lsr x12, x16, #56
    cbnz x12, Lfailed
    mov x13, sp
    and x12, x16, #0x7ffffffffff8
    cmp x12, x13
    b.ne Lfailed
    mov x12, x16
    xpacd x12
    mov x13, sp
    cmp x12, x13
    b.ne Lfailed
    xpacd x13
    cmp x12, x13
    b.ne Lfailed
    adr x12, Lfailed
    mov x13, x12
    paciza x12
    xpaci x12
    cmp x12, x13
    b.ne Lfailed
    str x30, [sp, #8]
    adr x12, Ltarget
    mov x13, x12
    pacia x12, x10
    autia x12, x10
    cmp x12, x13
    b.ne Lfailed
    pacia x12, x10
    blraa x12, x10
    adr x12, Ltarget
    pacib x12, x10
    blrab x12, x10
    ldr x30, [sp, #8]
    autda x16, x10
    ldr w8, [x16]
    mov x16, sp
    pacdb x16, x10
    autdb x16, x10
    ldr w9, [x16]
    add sp, sp, #16
    cmp w8, #42
    mov w11, #42
    ccmp w9, w11, #0, eq
    cset w0, ne
    ret
Lfailed:
    add sp, sp, #16
    mov w0, #1
    ret
Lbad:
    cmp w0, #4
    b.eq Lwrong_code_modifier
    cmp w0, #2
    b.ne Lwrong_key
    add x10, x10, #1
    autda x16, x10
    b Luse
Lwrong_key:
    autdb x16, x10
Luse:
    ldr w0, [x16]
    add sp, sp, #16
    ret
Lwrong_code_modifier:
    adr x12, Lfailed
    pacia x12, x10
    add x10, x10, #1
    braa x12, x10
Ltarget:
    ret
