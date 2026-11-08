.text
.globl _main
.p2align 2
_main:
    adr x8, Ltarget
    // BLRAAZ x8. Deliberately skip PACIZA so the guest must reject the target.
    .long 0xd63f091f
    ret
Ltarget:
    mov w0, #42
    ret
