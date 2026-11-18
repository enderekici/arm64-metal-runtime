.text
.p2align 2
.globl _main
_main:
    movz x0, #43
    sub sp, sp, #16
    adrp x1, dot_short_inputs@PAGE
    add x1, x1, dot_short_inputs@PAGEOFF
    ldr q0, [x1]
    ldr q1, [x1, #16]
    ldr q2, [x1, #32]
    sdot v0.2s, v1.8b, v2.8b
    str q0, [sp]
    ldr w3, [sp]
    movz w4, #74
    cmp w3, w4
    b.ne done
    ldr w3, [sp, #4]
    movz w4, #34
    cmp w3, w4
    b.ne done
    ldr x3, [sp, #8]
    cbnz x3, done
    movz x0, #41
    ldr q0, [x1, #48]
    ldr q1, [x1, #64]
    ldr q2, [x1, #80]
    udot v0.2s, v1.8b, v2.4b[1]
    str q0, [sp]
    ldr w3, [sp]
    movz w4, #8
    cmp w3, w4
    b.ne done
    ldr w3, [sp, #4]
    cmp w3, w4
    b.ne done
    ldr x3, [sp, #8]
    cbnz x3, done
    movz x0, #42
done:
    add sp, sp, #16
    ret

.data
.p2align 4
dot_short_inputs:
    .word 70, 30, 0x55555555, 0x55555555
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .word 0, 0, 0x55555555, 0x55555555
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .byte 1, 1, 1, 1, 1, 1, 1, 1
    .byte 0, 0, 0, 0, 2, 2, 2, 2
    .byte 0, 0, 0, 0, 0, 0, 0, 0
