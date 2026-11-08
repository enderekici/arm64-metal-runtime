.text
.globl _main
.p2align 2
_main:
    pacibsp
    sub sp, sp, #16
    autibsp
    add sp, sp, #16
    mov w0, #0
    ret
