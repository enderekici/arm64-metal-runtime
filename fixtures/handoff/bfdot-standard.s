// Arm standard BF16. All eight modes have EBF=0; AH controls the default NaN sign.
// Emit the raw destination (including the unused upper lanes), FPCR, and FPSR.
.arch armv8.6-a+bf16
.text
.p2align 2
.globl _main
_main:
    adrp x20, results@PAGE
    add x20, x20, results@PAGEOFF
    mov x21, x20
    adrp x12, modes@PAGE
    add x12, x12, modes@PAGEOFF
    mov w13, #8
Lmode:
    ldr x7, [x12], #8
    msr fpcr, x7
    adrp x6, cases@PAGE
    add x6, x6, cases@PAGEOFF
    mov w9, #rows
Lcase:
    ldr q24, [x6], #16
    ldr q25, [x6], #16
    ldr q31, [x6], #16
    mov w8, #0x9f
    movk w8, #0x800, lsl #16
    tst w9, #1
    csel w8, w8, wzr, ne
    msr fpsr, x8
.macro record dest, left, right
    mov v0.16b, v24.16b
    bfdot \dest, \left, \right
    str q0, [x20], #16
.endmacro
    record v0.4s, v25.8h, v31.8h
    record v0.2s, v25.4h, v31.4h
    record v0.4s, v25.8h, v31.2h[0]
    record v0.4s, v25.8h, v31.2h[1]
    record v0.4s, v25.8h, v31.2h[2]
    record v0.4s, v25.8h, v31.2h[3]
    record v0.2s, v25.4h, v31.2h[0]
    record v0.2s, v25.4h, v31.2h[1]
    record v0.2s, v25.4h, v31.2h[2]
    record v0.2s, v25.4h, v31.2h[3]
    record v0.4s, v0.8h, v31.8h
    record v0.2s, v25.4h, v0.2h[3]
    mrs x1, fpcr
    mrs x2, fpsr
    stp x1, x2, [x20], #16
    cmp x1, x7
    b.ne Lfail
    cmp x2, x8
    b.ne Lfail
    subs w9, w9, #1
    b.ne Lcase
    subs w13, w13, #1
    b.ne Lmode

    // First full-vector result: round-to-odd from either side of one,
    // opposite separately overflowing products, and products flushed before addition.
    ldp w0, w1, [x21]
    movz w2, #0x3f80, lsl #16
    add w3, w2, #1
    cmp w0, w3
    b.ne Lfail
    sub w3, w2, #1
    cmp w1, w3
    b.ne Lfail
    ldp w0, w1, [x21, #8]
    movz w2, #0x7fc0, lsl #16
    cmp w0, w2
    b.ne Lfail
    cbnz w1, Lfail
    mov x0, #1
    mov x1, x21
    sub x2, x20, x21
    mov x16, #4
    svc #0x80
    b.cs Lfail
    cmp x0, x2
    b.ne Lfail
    mov x0, #0
    b Lexit
Lfail:
    mov x0, #1
Lexit:
    mov x16, #1
    svc #0x80

.const
.p2align 4
modes:
    .quad 0, 2, 0x00400000, 0x00400002, 0x01800000, 0x01800002, 0x03c00000, 0x03c00002
cases:
    // 1 +/- min normal, maxBF*maxBF - maxBF*maxBF, and two individually underflowing products.
    .long 0x3f800000, 0x3f800000, 0, 0
    .long 0x00000080, 0x00008080, 0xff7f7f7f, 0x00800080
    .long 0x00003f80, 0x00003f80, 0x7f7f7f7f, 0x3f003f00
    // Signed zeros, subnormal operands/addends, sNaN, qNaN, infinity * zero.
    .long 0x80000000, 0x00000001, 0xff800001, 0x7fc12345
    .long 0x80008000, 0x807f007f, 0x7f813f80, 0x7f800000
    .long 0x3f803f80, 0xbf807f7f, 0x00003f80, 0x00007f80
    // Cancellation across adjacent exponents, an exact cancellation, smallest normal, overflow.
    .long 0xbf800001, 0x40000000, 0x00800001, 0x7f7fffff
    .long 0x00003f80, 0xbf80bf80, 0x00008080, 0x7f7f7f7f
    .long 0x00003f80, 0x3f803f80, 0x00003f80, 0x40004000
    // Probe the exact/short and jammed/long alignment boundary at exponent gaps 31..34.
    .long 0x3f800000, 0x3f800001, 0xbf800000, 0xbf800001
    .long 0x00002fff, 0x0000af80, 0x00002f7f, 0x0000af00
    .long 0x00003f80, 0x00003f80, 0x00003f80, 0x00003f80
    // Deterministic raw operands include both large and small exponents.
    .set seed, 0x789a1234
    .rept 64 * 12
      .set seed, (seed * 1664525 + 1013904223) & 0xffffffff
      .long seed
    .endr
    .set rows, (. - cases) / 48
.zerofill __DATA,__bss,results,8 * rows * (12 * 16 + 16),4
