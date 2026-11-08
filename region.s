.text
.globl _main
.p2align 2
_main:
    mov w0, #0
    .rept 260
    add w0, w0, #1
    .endr
    // Exercise more than one million translated steps across dispatch quanta.
    movz w1, #0x27c0
    movk w1, #9, lsl #16
Lloop:
    subs w1, w1, #1
    b.ne Lloop
    ret
