.text
.globl _main
_main:
    stp x19, x30, [sp, #-16]!
    bl .Lhelper
    ldp x19, x30, [sp], #16
    ret
.Lhelper:
    movz x0, #42
    ret
