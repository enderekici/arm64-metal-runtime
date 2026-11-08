.text
.p2align 2
.globl _main
_main:
    movz x1, #7
    movz x2, #9
    movz x3, #5

    // Accumulate and subtract the full product without changing NZCV.
    cmp x1, #7
    madd x4, x1, x2, x3
    b.ne .Lfail
    movz x5, #68
    cmp x4, x5
    b.ne .Lfail
    msub x6, x1, x2, x3
    cmp x6, #-58
    b.ne .Lfail

    // MUL and MNEG are MADD/MSUB with ZR as the accumulator.
    mul x7, x1, x2
    movz x5, #63
    cmp x7, x5
    b.ne .Lfail
    mneg x8, x1, x2
    cmp x8, #-63
    b.ne .Lfail
    msub x9, x1, x2, xzr
    cmp x9, x8
    b.ne .Lfail

    // Destination aliasing reads every source before writing Rd.
    madd x7, x7, x2, x3
    movz x5, #572
    cmp x7, x5
    b.ne .Lfail
    madd xzr, x1, x2, x3
    cmp x3, #5
    b.ne .Lfail

    // W arithmetic wraps at 32 bits and zero extends the result.
    movn w10, #0
    movz w11, #2
    movz w12, #3
    madd w13, w10, w11, w12
    cmp x13, #1
    b.ne .Lfail
    msub w14, w11, w11, w12
    cmp w14, #-1
    b.ne .Lfail

    // X multiplication wraps at 64 bits; Rn/Rm can be ZR.
    movn x15, #0
    movz x16, #2
    movz x17, #5
    madd x18, x15, x16, x17
    movz x5, #3
    cmp x18, x5
    b.ne .Lfail
    madd x19, xzr, x16, x17
    cmp x19, x17
    b.ne .Lfail
    msub x20, x16, x17, xzr
    cmp x20, #-10
    b.ne .Lfail

    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
