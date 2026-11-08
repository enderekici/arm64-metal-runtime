.text
.globl _main
_main:
    mov x0, #42
#if FP_CASE == 1
    fmov d0, #0.75
    fneg d2, d0
    fmov d3, #-0.75
    fcmp d2, d3
    csel x0, x0, xzr, eq
#elif FP_CASE == 2
    fmov d0, #0.75
    fmov d1, #0.5
    fcmp d0, d1
    csel x0, x0, xzr, gt
#elif FP_CASE == 3
    fmov d0, #-0.75
    fcmp d0, #0.0
    csel x0, x0, xzr, lt
#elif FP_CASE == 4
    fmov s0, #0.75
    fmov s1, #0.5
    fcmpe s0, s1
    csel x0, x0, xzr, gt
#elif FP_CASE == 5
    fmov s0, #-0.75
    fcmpe s0, #0.0
    csel x0, x0, xzr, lt
#else
#error FP_CASE must select a scalar FP comparison fixture.
#endif
    ret
