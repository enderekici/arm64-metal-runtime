.text
.p2align 2
.globl _main
_main:
    adrp x1, vector_input@PAGE
    add x1, x1, vector_input@PAGEOFF
    ldr q0, [x1]
    rev32 v0.8b, v0.8b
    umov x2, v0.d[0]
    movz x3, #0x0203
    movk x3, #0x0001, lsl #16
    movk x3, #0x0607, lsl #32
    movk x3, #0x0405, lsl #48
    cmp x2, x3
    b.ne fail
    umov x2, v0.d[1]
    cbnz x2, fail
    mov x0, #42
    ret
fail:
    mov x0, #7
    ret

.section __DATA,__data
.p2align 4
vector_input:
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
