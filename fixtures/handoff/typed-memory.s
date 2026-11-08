.text
.p2align 2
.globl _typed_memory
_typed_memory:
  str w1, [x0, #4]
  ldr w3, [x0, #4]
  str w3, [x0, #8]
  str x2, [x0, #16]
  ldr x4, [x0, #16]
  str x4, [x0, #24]
  sub sp, sp, #16
  str x2, [sp]
  ldr x5, [sp]
  add sp, sp, #16
  str x5, [x0, #32]
  ret
