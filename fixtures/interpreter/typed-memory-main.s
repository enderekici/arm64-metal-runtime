.text
.globl _main
_main:
    movz w0, #42
    str w0, [sp, #4]
    ldr w1, [sp, #4]
    str x1, [sp, #8]
    ldr x0, [sp, #8]
    ret
