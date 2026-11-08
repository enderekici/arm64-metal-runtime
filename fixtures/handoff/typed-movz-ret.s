.text
.p2align 2
.globl _movz_x_lsl0
_movz_x_lsl0:
  movz x0, #0x1234
  ret

.globl _movz_x_lsl16
_movz_x_lsl16:
  movz x0, #0x1234, lsl #16
  ret

.globl _movz_x_lsl32
_movz_x_lsl32:
  movz x0, #0x1234, lsl #32
  ret

.globl _movz_x_lsl48
_movz_x_lsl48:
  movz x0, #0x1234, lsl #48
  ret

.globl _movz_w_lsl0
_movz_w_lsl0:
  movz w0, #0x1234
  ret

.globl _movz_w_lsl16
_movz_w_lsl16:
  movz w0, #0x1234, lsl #16
  ret

.globl _movz_w_zero_extends
_movz_w_zero_extends:
  movz x0, #0x7777, lsl #48
  movz w0, #0x1234, lsl #16
  ret

.globl _movz_discard_xzr
_movz_discard_xzr:
  movz x0, #0x5678
  movz xzr, #0x1234, lsl #32
  ret

.globl _movz_discard_wzr
_movz_discard_wzr:
  movz x0, #0x5678
  movz wzr, #0x1234, lsl #16
  ret

.globl _movk_outside_slice
_movk_outside_slice:
  movz x0, #0x5678
  movk x0, #0x1234, lsl #16
  ret
