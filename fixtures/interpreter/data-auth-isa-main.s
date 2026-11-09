.text
.globl _main
.p2align 2
_main:
    adr x0, class_data
    mov x1, #0x8fd
    pacda x0, x1
    bic x2, x0, #0x7ffffffffffff8
    mov x3, x0
    autda x3, x1
    orr x4, x2, x3
    adr x5, class_data
    cmp x4, x5
    b.ne 1f
    mov w0, #42
    ret
1:
    brk #0xc471

.text
.p2align 3
class_data:
    .quad 0
