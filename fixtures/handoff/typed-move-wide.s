.text
.p2align 2
.globl _movn_x
_movn_x:
  movn x0, #0x1234, lsl #16
  ret

.globl _movn_w
_movn_w:
  movn w0, #0x1234, lsl #16
  ret

.globl _movk_x
_movk_x:
  movz x0, #0x5678
  movk x0, #0xabcd, lsl #32
  ret

.globl _movk_w
_movk_w:
  movz x0, #0x7777, lsl #48
  movk w0, #0xabcd, lsl #16
  ret

.globl _movk_discard_xzr
_movk_discard_xzr:
  movz x0, #0x5678
  movk xzr, #0xabcd, lsl #32
  ret

.globl _movk_discard_wzr
_movk_discard_wzr:
  movz x0, #0x5678
  movk wzr, #0xabcd, lsl #16
  ret
