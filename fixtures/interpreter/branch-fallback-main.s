.text
.globl _main
_main:
    movz x0, #7
    b .Lfallback
    movz x0, #99
.Lfallback:
    nop
    movz x0, #42
    ret
