.text
.globl _main
_main:
    movz x0, #7
    b .Lreturn
    movz x0, #99
.Lreturn:
    ret
