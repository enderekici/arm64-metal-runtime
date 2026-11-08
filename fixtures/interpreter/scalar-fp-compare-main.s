.text
.globl _main
_main:
    mov x0, #42
    fmov d0, #0.75
    fmov d1, #0.5
    fneg d2, d0
    fmov d3, #-0.75
    fcmp d2, d3
    csel x0, x0, xzr, eq
    fcmp d0, d1
    csel x0, x0, xzr, gt
    fcmp d2, #0.0
    csel x0, x0, xzr, lt
    fmov s0, #0.75
    fmov s1, #0.5
    fcmpe s0, s1
    csel x0, x0, xzr, gt
    fmov s2, #-0.75
    fcmpe s2, #0.0
    csel x0, x0, xzr, lt
    ret
