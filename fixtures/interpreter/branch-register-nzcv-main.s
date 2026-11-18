.text
.p2align 2
.globl _main
_main:
    stp x19, x30, [sp, #-16]!
    movz x1, #0
    cmp x1, #0
    adr x10, .Lleaf
    blr x10
    cset w0, eq
    add x0, x0, #41
    ldp x19, x30, [sp], #16
    ret
.Lleaf:
    ret
