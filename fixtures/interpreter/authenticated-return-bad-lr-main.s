.text
.globl _main
.p2align 2
_main:
    pacibsp
    mov x30, xzr
    autibsp
    mov w0, #0
    ret
