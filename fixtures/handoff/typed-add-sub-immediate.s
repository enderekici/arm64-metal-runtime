.text
.p2align 2
.globl _add_x_shifted
_add_x_shifted:
    movz x0, #5
    add x0, x0, #0x123, lsl #12
    ret

.globl _sub_x_shifted
_sub_x_shifted:
    movz x0, #0x2345, lsl #16
    sub x0, x0, #0x123, lsl #12
    ret

.globl _add_w_wrap
_add_w_wrap:
    movz w0, #0xffff
    add w0, w0, #1
    ret

.globl _sub_w_shifted
_sub_w_shifted:
    movz w0, #0x1234, lsl #16
    sub w0, w0, #0x123, lsl #12
    ret

.globl _add_sub_preserve_nzcv
_add_sub_preserve_nzcv:
    movz x1, #5
    cmp x1, #0
    add x0, x1, #2
    sub x0, x1, #1
    mrs x0, nzcv
    ret
