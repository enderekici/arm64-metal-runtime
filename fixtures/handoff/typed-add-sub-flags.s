.text
.p2align 2
.globl _adds_x_overflow_flags
_adds_x_overflow_flags:
    mov x1, x0
    adds xzr, x1, #1
    mrs x0, nzcv
    ret

.globl _subs_x_underflow_flags
_subs_x_underflow_flags:
    mov x1, x0
    subs xzr, x1, #1
    mrs x0, nzcv
    ret

.globl _cmp_x_equal_flags
_cmp_x_equal_flags:
    cmp x0, #1, lsl #12
    mrs x0, nzcv
    ret

.globl _cmn_x_carry_flags
_cmn_x_carry_flags:
    cmn x0, #1, lsl #12
    mrs x0, nzcv
    ret

.globl _adds_w_carry_flags
_adds_w_carry_flags:
    adds wzr, w0, #1
    mrs x0, nzcv
    ret

.globl _subs_w_underflow_flags
_subs_w_underflow_flags:
    subs wzr, w0, #1
    mrs x0, nzcv
    ret

.globl _cmp_sp_zero_flags
_cmp_sp_zero_flags:
    cmp sp, #0
    mrs x0, nzcv
    ret

.globl _adds_w_zero_extend
_adds_w_zero_extend:
    adds w0, w0, #1
    ret

.globl _sp_update
_sp_update:
    mov x8, x0
    mov x9, sp
    add x10, sp, #2, lsl #12
    sub sp, sp, #1, lsl #12
    sub x11, sp, x9
    sub x10, x10, x9
    stp x10, x11, [x8]
    add sp, sp, #1, lsl #12
    ret

.globl _wsp_zero_extend
_wsp_zero_extend:
    mov x9, sp
    movz w10, #0x8000, lsl #16
    add wsp, w10, #0
    add x0, sp, #1
    mov sp, x9
    ret
