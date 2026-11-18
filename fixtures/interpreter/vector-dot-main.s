.text
.p2align 2
.globl _main
_main:
    movz x0, #42
    sub sp, sp, #16
    adrp x1, dot_inputs@PAGE
    add x1, x1, dot_inputs@PAGEOFF
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    sdot v0.4s, v1.16b, v2.16b
    str q0, [sp]
    ldr w3, [sp, #0]
    movz w4, #0xfe9e
    movk w4, #0xffff, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x03e9
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x00ab
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0xff49
    movk w4, #0xffff, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    udot v0.4s, v1.16b, v2.16b
    str q0, [sp]
    ldr w3, [sp, #0]
    movz w4, #0x059e
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x03e9
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x00ab
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0x4149
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    sdot v0.4s, v1.16b, v2.4b[0]
    str q0, [sp]
    ldr w3, [sp, #0]
    movz w4, #0xfe9e
    movk w4, #0xffff, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x01c9
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x005b
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0xfff1
    movk w4, #0xffff, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    sdot v0.4s, v1.16b, v2.4b[1]
    str q0, [sp]
    ldr w3, [sp, #0]
    movz w4, #0xfca6
    movk w4, #0xffff, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x03e9
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x0083
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0xffb9
    movk w4, #0xffff, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    udot v0.4s, v1.16b, v2.4b[2]
    str q0, [sp]
    ldr w3, [sp, #0]
    movz w4, #0x11ae
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x0609
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x00ab
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0x3181
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    udot v0.4s, v1.16b, v2.4b[3]
    str q0, [sp]
    ldr w3, [sp, #0]
    movz w4, #0x17b6
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x0829
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x00d3
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0x4149
    movk w4, #0x0000, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    sdot v1.4s, v1.16b, v2.16b
    str q1, [sp]
    ldr w3, [sp, #0]
    movz w4, #0xff8f
    movk w4, #0x80fe, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x06d0
    movk w4, #0x7f02, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x0386
    movk w4, #0x0102, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0xfc0f
    movk w4, #0xfefd, lsl #16
    cmp w3, w4
    b.ne fail
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    udot v2.4s, v1.16b, v2.16b
    str q2, [sp]
    ldr w3, [sp, #0]
    movz w4, #0x0a93
    movk w4, #0x0304, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #4]
    movz w4, #0x0cd6
    movk w4, #0x0708, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #8]
    movz w4, #0x0d90
    movk w4, #0x0b0c, lsl #16
    cmp w3, w4
    b.ne fail
    ldr w3, [sp, #12]
    movz w4, #0x5226
    movk w4, #0x0f10, lsl #16
    cmp w3, w4
    b.ne fail
    add sp, sp, #16
    ret
fail:
    movz x0, #43
    add sp, sp, #16
    ret
.data
.p2align 4
dot_inputs:
    .word 17, 29, 41, 53
    .word 0x80ff0102, 0x7f020304, 0x01020304, 0xfefdfcfb
    .word 0x03040506, 0x0708090a, 0x0b0c0d0e, 0x0f101112
