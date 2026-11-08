.text
.globl _main
.p2align 2
_main:
    pacibsp
    sub sp, sp, #16
    autibsp
    add sp, sp, #16
    mov x19, x30
    bl _leaf
    mov x30, x19
    ret

.p2align 2
_leaf:
    ret
