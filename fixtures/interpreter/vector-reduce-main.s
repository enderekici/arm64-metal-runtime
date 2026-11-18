.text
.p2align 2
.globl _main
_main:
    movz x0, #42
    sub sp, sp, #16
    adrp x1, vector_values@PAGE
    add x1, x1, vector_values@PAGEOFF
    ldr q0, [x1]
    addv b2, v0.16b
    str q2, [sp]
    ldr w3, [sp]
    cmp w3, #16
    b.ne fail
    addv b2, v0.8b
    str q2, [sp]
    ldr w3, [sp]
    cmp w3, #8
    b.ne fail
    addv h2, v0.4h
    str q2, [sp]
    ldr w3, [sp]
    movz w4, #0x0404
    cmp w3, w4
    b.ne fail
    addv h2, v0.8h
    str q2, [sp]
    ldr w3, [sp]
    movz w4, #0x0808
    cmp w3, w4
    b.ne fail
    addv s2, v0.4s
    str q2, [sp]
    ldr w3, [sp]
    movz w4, #0x0404, lsl #16
    movk w4, #0x0404
    cmp w3, w4
    b.ne fail
    addv b0, v0.16b
    str q0, [sp]
    ldr w3, [sp]
    cmp w3, #16
    b.ne fail
    add sp, sp, #16
    ret
fail:
    movz x0, #43
    ret

.data
.p2align 4
vector_values:
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .byte 1, 1, 1, 1, 1, 1, 1, 1
