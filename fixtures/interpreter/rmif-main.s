.text
.globl _main
.p2align 2
_main:
    rmif xzr, #0, #1
    mov x0, #42
    ret
