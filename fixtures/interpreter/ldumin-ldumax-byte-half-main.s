.text
.globl _main
_main:
  sub sp, sp, #32
  mov x9, sp
  mov w10, #0x44
  str w10, [x9]
  mov w11, #0x22
  lduminb w11, w12, [x9]
  cmp w12, #0x44
  b.ne fail
  ldr w13, [x9]
  and w13, w13, #0xff
  cmp w13, #0x22
  b.ne fail
  mov w10, #0x22
  mov w11, #0x44
  ldumaxb w11, w12, [x9]
  cmp w12, #0x22
  b.ne fail
  ldr w13, [x9]
  and w13, w13, #0xff
  cmp w13, #0x44
  b.ne fail

  mov w10, #0x44
  str w10, [x9, #4]
  add x14, x9, #4
  mov w11, #0x22
  lduminh w11, w12, [x14]
  cmp w12, #0x44
  b.ne fail
  ldr w13, [x9, #4]
  and w13, w13, #0xffff
  cmp w13, #0x22
  b.ne fail
  mov w10, #0x22
  str w10, [x9, #4]
  mov w11, #0x44
  ldumaxh w11, w12, [x14]
  cmp w12, #0x22
  b.ne fail
  ldr w13, [x9, #4]
  and w13, w13, #0xffff
  cmp w13, #0x44
  b.ne fail

  mov w10, #0x80
  str w10, [x9, #8]
  add x14, x9, #8
  mov w11, #0x7f
  ldsminb w11, w12, [x14]
  cmp w12, #0x80
  b.ne fail
  ldr w13, [x9, #8]
  and w13, w13, #0xff
  cmp w13, #0x80
  b.ne fail
  mov w10, #0x80
  str w10, [x9, #8]
  mov w11, #0x7f
  ldsmaxb w11, w12, [x14]
  cmp w12, #0x80
  b.ne fail
  ldr w13, [x9, #8]
  and w13, w13, #0xff
  cmp w13, #0x7f
  b.ne fail

  mov w10, #0x8000
  str w10, [x9, #12]
  add x14, x9, #12
  mov w11, #0x7fff
  ldsminh w11, w12, [x14]
  cmp w12, #0x8000
  b.ne fail
  ldr w13, [x9, #12]
  and w13, w13, #0xffff
  cmp w13, #0x8000
  b.ne fail
  mov w10, #0x8000
  str w10, [x9, #12]
  mov w11, #0x7fff
  ldsmaxh w11, w12, [x14]
  cmp w12, #0x8000
  b.ne fail
  ldr w13, [x9, #12]
  and w13, w13, #0xffff
  mov w15, #0x7fff
  cmp w13, w15
  b.ne fail
  mov w0, #42
  b done
fail:
  mov w0, #2
done:
  add sp, sp, #32
  ret
