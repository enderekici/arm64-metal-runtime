.text
.globl _main
_main:
    movz x0, #0x42d8
    movz x1, #42
    str x1, [x0]
    ret
