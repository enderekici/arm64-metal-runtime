.text
.globl _main
.p2align 2
_main:
    cmp xzr, xzr
    b.ne LafterInvert
    cfinv
LafterInvert:
    b.cs Lfail
    b.ne Lfail

    movn x9, #0
    lsr x9, x9, #1
    adds x9, x9, #1
    cfinv
    b.cc Lfail
    b.pl Lfail
    b.vc Lfail
    b.eq Lfail
    mov x0, #42
    ret

Lfail:
    mov x0, #1
    ret
