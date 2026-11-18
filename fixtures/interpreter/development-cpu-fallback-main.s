.text
.globl _main
.p2align 2
_main:
    cmp xzr, xzr
    cfinv
    b.cs fail
    b.ne fail

    movn x9, #0
    lsr x9, x9, #1
    adds x9, x9, #1
    cfinv
    b.cc fail
    b.pl fail
    b.vc fail
    b.eq fail
    mov x0, #42
    ret

fail:
    mov x0, #1
    ret
