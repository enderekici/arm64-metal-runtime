.text
.p2align 2
.globl _main
_main:
    movz w0, #42
    add sp, sp, #1, lsl #12
    sub sp, sp, #1, lsl #12
    cmp x0, #42
    b .Lcheck_flags
.Lcheck_flags:
    madd x12, xzr, xzr, xzr
    b.ne .Lwrong_flags
    ret
.Lwrong_flags:
    movz w0, #99
    ret
