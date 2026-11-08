.text
.globl _main
.p2align 2
_main:
    .long 0xd503245f // BTI c
    sub sp, sp, #256
    stp x29, x30, [sp, #240]
    adr x9, Lbytes
    add x9, x9, #1
    ld1 {v0.4s, v1.4s}, [x9], #32
    ldur q0, [x9, #-32]
    rev64 v1.16b, v1.16b
    ext v1.16b, v1.16b, v0.16b, #8
    add x9, sp, #57
    st1 {v1.16b}, [x9]
    rev32 v0.16b, v0.16b
    stur q0, [sp, #17]
    mov w8, #0x53
    dup v1.4h, w8
    adr x9, Lshifts
    ldr d2, [x9]
    ushl v1.4h, v1.4h, v2.4h
    add v1.4h, v1.4h, v2.4h
    mla v1.4h, v1.4h, v2.4h
    shl v1.4h, v1.4h, #1
    usra v1.4h, v2.4h, #3
    orr v1.8b, v1.8b, v2.8b
    bic v1.4h, #0xfe
    uzp1 v1.8b, v1.8b, v1.8b
    add x9, sp, #33
    str d1, [x9]
    movi d3, #0x000000ffffffff
    mov w9, #41
    str d3, [sp, w9, uxtw]
    mov w4, #0x80000000
    add w5, w4, w4, asr #31
    sub x4, x5, w4, sxtw #1
    ldrsw x6, [sp, w9, uxtw]
    add x4, x4, x6
    stur x4, [sp, #49]
    mov w0, #1
    add x1, sp, #17
    mov x2, #56
    bl _write
    cmp x0, #56
    cset w0, ne
    mov w10, w0
    adr x9, Lleft
    ldr q2, [x9]
    adr x9, Lright
    ldr q0, [x9]
    adr x9, Lthird
    ldr q3, [x9]
    adr x9, Lfourth
    ldr q1, [x9]
    adr x9, Lfifth
    ldr q5, [x9]
    adr x9, Lsixth
    ldr q7, [x9]
    shl d6, d5, #37
    add d5, d5, d6
    str d5, [sp, #80]
    umull v4.8h, v2.8b, v0.8b
    str q4, [sp, #88]
    umlal v4.8h, v3.8b, v1.8b
    str q4, [sp, #104]
    umlal2 v4.8h, v2.16b, v0.16b
    str q4, [sp, #120]
    uaddlv h2, v2.16b
    umov w11, v2.h[0]
    str w11, [sp, #136]
    uaddlv s4, v4.8h
    umov w11, v4.s[0]
    str w11, [sp, #140]
    uaddl v2.4s, v2.4h, v6.4h
    str q2, [sp, #144]
    zip1 v4.2s, v2.2s, v4.2s
    str q4, [sp, #160]
    umull v2.2d, v5.2s, v7.s[1]
    str q2, [sp, #176]
    mls v5.2s, v2.2s, v7.s[0]
    str q5, [sp, #192]
    mov v2.s[1], v2.s[2]
    str q2, [sp, #208]
    mov w0, v5.s[0]
    str w0, [sp, #224]
    str wzr, [sp, #228]
    mov x0, v5.d[0]
    str x0, [sp, #232]
    mov w0, #1
    add x1, sp, #80
    mov x2, #160
    bl _write
    cmp x0, #160
    cset w0, ne
    orr w0, w0, w10
    cbnz w0, Ldone
    adr x9, Lleft
    ldp q31, q0, [x9]
    adr x9, Lindices
    ldr q2, [x9]
    tbl v3.16b, {v31.16b, v0.16b}, v2.16b
    tbx v31.8b, {v31.16b, v0.16b}, v2.8b
    stp q3, q31, [sp, #80]
    mov w0, #1
    add x1, sp, #80
    mov x2, #32
    bl _write
    cmp x0, #32
    cset w0, ne
    cbnz w0, Ldone
    adr x9, Lindices
    ldr q2, [x9]
    uminv b3, v2.16b
    umaxv b4, v2.8b
    sminv b5, v2.16b
    smaxv b6, v2.16b
    stp q3, q4, [sp, #80]
    stp q5, q6, [sp, #112]
    adr x9, Lleft
    ldr q1, [x9]
    cmhi v3.16b, v2.16b, v1.16b
    cmhs v4.8b, v2.8b, v1.8b
    cmgt v5.8h, v2.8h, v1.8h
    cmge v6.2d, v2.2d, v2.2d
    stp q3, q4, [sp, #144]
    stp q5, q6, [sp, #176]
    mov v1.b[0], v2.b[0]
    cmeq v7.16b, v2.16b, v1.16b
    str q7, [sp, #208]
    mov w0, #1
    add x1, sp, #80
    mov x2, #144
    bl _write
    cmp x0, #144
    cset w0, ne
    cbnz w0, Ldone
    ldr q0, Lindices
    ldr d1, Lleft
    ldr s2, Lright
    stp q0, q1, [sp, #80]
    str q2, [sp, #112]
    mov w0, #1
    add x1, sp, #80
    mov x2, #48
    bl _write
    cmp x0, #48
    cset w0, ne
    cbnz w0, Ldone
    ldr q0, Lindices
    ldr q1, Lleft
    ldr q2, Lindices
    .arch_extension crypto
    aese v0.16b, v1.16b
    aesd v2.16b, v1.16b
    aesmc v3.16b, v0.16b
    aesimc v4.16b, v2.16b
    stp q0, q2, [sp, #80]
    stp q3, q4, [sp, #112]
    dup b5, v0.b[15]
    dup v6.8b, v2.b[10]
    stp q5, q6, [sp, #144]
    mov w0, #1
    add x1, sp, #80
    mov x2, #96
    bl _write
    cmp x0, #96
    cset w0, ne
Ldone:
    ldp x29, x30, [sp, #240]
    add sp, sp, #256
    ret
Lbytes:
    .ascii "!0123456789abcdefFEDCBA9876543210"

.p2align 3
Lshifts:
    .short -1, 0, 1, 4
.p2align 4
Lleft:
    .byte 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
Lright:
    .byte 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17
Lthird:
    .byte 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18
Lfourth:
    .byte 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
Lfifth:
    .word 0x12345678, 0x23456789, 0x3456789a, 0x456789ab
Lsixth:
    .word 3, 4, 5, 6
Lindices:
    .byte 0, 15, 16, 31, 32, 255, 1, 17, 30, 29, 28, 27, 26, 25, 24, 23
