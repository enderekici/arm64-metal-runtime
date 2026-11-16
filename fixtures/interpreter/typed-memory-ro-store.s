.text
.globl _main
_main:
    adr x0, _main
    movz x1, #42
    str x1, [x0]
    ret
