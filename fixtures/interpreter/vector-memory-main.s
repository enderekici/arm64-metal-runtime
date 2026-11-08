.text
.globl _main
_main:
    sub sp, sp, #384
    adr x0, vector_data
    ldr s1, [x0]
    str q1, [sp, #192]
    ldr d2, [x0, #8]
    str q2, [sp, #208]
    ldr q3, [x0, #16]
    str q3, [sp, #16]
    ldur s4, [x0, #32]
    stur s4, [sp, #32]
    ldur d5, [x0, #40]
    stur d5, [sp, #40]
    ldur q6, [x0, #48]
    stur q6, [sp, #48]

    adr x1, vector_data
    ldr q7, [x1], #16
    str q7, [sp, #64]
    ldr q8, [x1, #16]!
    str q8, [sp, #80]

    mov x2, #0
    ldr b9, [x1, w2, uxtw]
    ldr h10, [x1, w2, sxtw #1]
    ldr s11, [x1, w2, uxtw #2]
    ldr d12, [x1, x2, lsl #3]
    ldr q13, [x1, x2, sxtx #4]
    mov x3, #160
    str q13, [sp, x3]

    ldr s14, literal_s
    str s14, [sp, #128]
    ldr d15, literal_d
    str d15, [sp, #136]
    ldr q16, literal_q
    str q16, [sp, #144]

    str s1, [sp, #224]
    str d2, [sp, #232]
    add x4, sp, #256
    str q3, [x4], #16
    add x5, sp, #272
    cmp x4, x5
    b.ne fail10
    add x4, sp, #288
    str q3, [x4, #-16]!
    add x5, sp, #272
    cmp x4, x5
    b.ne fail11

    mov x3, #320
    str b9, [sp, x3]
    mov x3, #322
    str h10, [sp, x3]
    mov x3, #324
    str s11, [sp, x3]
    mov x3, #328
    str d12, [sp, x3]

    ldr w9, [sp, #192]
    movz w10, #0x7788
    movk w10, #0x5566, lsl #16
    cmp w9, w10
    b.ne fail1
    ldr x9, [sp, #200]
    cbnz x9, fail2
    ldr w9, [sp, #208]
    movz w10, #0xff00
    movk w10, #0xddee, lsl #16
    cmp w9, w10
    b.ne fail3
    ldr x9, [sp, #216]
    cbnz x9, fail4
    ldr x9, [sp, #16]
    movz x10, #0xcdef
    movk x10, #0x89ab, lsl #16
    cmp w9, w10
    b.ne fail5
    ldr x9, [sp, #24]
    movz w10, #0x3210
    movk w10, #0x7654, lsl #16
    cmp w9, w10
    b.ne fail6
    ldr x9, [sp, #40]
    movz x10, #0x3210
    movk x10, #0x7654, lsl #16
    cmp w9, w10
    b.ne fail7
    ldr x9, [sp, #56]
    movz x10, #0x3210
    movk x10, #0x7654, lsl #16
    cmp w9, w10
    b.ne fail8
    ldr x9, [sp, #128]
    movz x10, #0x1357
    movk x10, #0x2468, lsl #16
    cmp w9, w10
    b.ne fail9
    ldr x9, [sp, #136]
    movz x10, #0xdef0
    movk x10, #0x9abc, lsl #16
    cmp w9, w10
    b.ne fail9
    ldr w9, [sp, #224]
    movz w10, #0x7788
    movk w10, #0x5566, lsl #16
    cmp w9, w10
    b.ne fail12
    ldr x9, [sp, #232]
    movz x10, #0xff00
    movk x10, #0xddee, lsl #16
    movk x10, #0xbbcc, lsl #32
    movk x10, #0x99aa, lsl #48
    cmp x9, x10
    b.ne fail13
    ldr x9, [sp, #256]
    movz x10, #0xcdef
    movk x10, #0x89ab, lsl #16
    cmp w9, w10
    b.ne fail14
    ldr x9, [sp, #264]
    movz x10, #0x3210
    movk x10, #0x7654, lsl #16
    movk x10, #0xba98, lsl #32
    movk x10, #0xfedc, lsl #48
    cmp x9, x10
    b.ne fail15
    ldr x9, [sp, #272]
    movz x10, #0xcdef
    movk x10, #0x89ab, lsl #16
    cmp w9, w10
    b.ne fail16
    ldr x9, [sp, #280]
    movz x10, #0x3210
    movk x10, #0x7654, lsl #16
    movk x10, #0xba98, lsl #32
    movk x10, #0xfedc, lsl #48
    cmp x9, x10
    b.ne fail17
    ldrb w9, [sp, #320]
    mov w10, #0xef
    cmp w9, w10
    b.ne fail18
    ldrh w9, [sp, #322]
    movz w10, #0xcdef
    cmp w9, w10
    b.ne fail19
    ldr w9, [sp, #324]
    movz w10, #0xcdef
    movk w10, #0x89ab, lsl #16
    cmp w9, w10
    b.ne fail20
    ldr x9, [sp, #328]
    movz x10, #0xcdef
    movk x10, #0x89ab, lsl #16
    movk x10, #0x4567, lsl #32
    movk x10, #0x0123, lsl #48
    cmp x9, x10
    b.ne fail23
    ldr x9, [sp, #160]
    movz x10, #0xcdef
    movk x10, #0x89ab, lsl #16
    movk x10, #0x4567, lsl #32
    movk x10, #0x0123, lsl #48
    cmp x9, x10
    b.ne fail21
    ldr x9, [sp, #168]
    movz x10, #0x3210
    movk x10, #0x7654, lsl #16
    movk x10, #0xba98, lsl #32
    movk x10, #0xfedc, lsl #48
    cmp x9, x10
    b.ne fail22

    b _vector_memory_fallback
fail1:
    mov w0, w9
    b fail_return
fail2:
    mov w0, #92
    b fail_return
fail3:
    mov w0, #93
    b fail_return
fail4:
    mov w0, #94
    b fail_return
fail5:
    mov w0, #95
    b fail_return
fail6:
    mov w0, #96
    b fail_return
fail7:
    mov w0, #97
    b fail_return
fail8:
    mov w0, #98
    b fail_return
fail9:
    mov w0, w9
    b fail_return
fail10:
    mov w0, #110
    b fail_return
fail11:
    mov w0, #111
    b fail_return
fail12:
    mov w0, #112
    b fail_return
fail13:
    mov w0, #113
    b fail_return
fail14:
    mov w0, #114
    b fail_return
fail15:
    mov w0, #115
    b fail_return
fail16:
    mov w0, #116
    b fail_return
fail17:
    mov w0, #117
    b fail_return
fail18:
    mov w0, #118
    b fail_return
fail19:
    mov w0, #119
    b fail_return
fail20:
    mov w0, #120
    b fail_return
fail21:
    mov w0, #121
    b fail_return
fail22:
    mov w0, #122
    b fail_return
fail23:
    mov w0, #123
fail_return:
    add sp, sp, #384
    ret
.globl _vector_memory_fallback
_vector_memory_fallback:
    nop
    mov w0, #42
    add sp, sp, #384
    ret

.align 4
vector_data:
    .quad 0x1122334455667788, 0x99aabbccddeeff00
    .quad 0x0123456789abcdef, 0xfedcba9876543210
    .quad 0x0123456789abcdef, 0xfedcba9876543210
    .quad 0x0123456789abcdef, 0xfedcba9876543210
literal_s:
    .long 0x24681357
literal_d:
    .quad 0x765432109abcdef0
literal_q:
    .quad 0x0123456789abcdef, 0xfedcba9876543210
