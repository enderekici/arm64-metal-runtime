.text
.p2align 2
.globl _main
_main:
    movn x1, #0
    movz x2, #5

    // AND and BIC use the same shifted value, with only BIC inverting it.
    and x3, x1, x2, lsl #2
    cmp x3, #20
    b.ne .Lfail
    bic x4, x1, x2, lsl #2
    eor x5, x4, x1
    cmp x5, #20
    b.ne .Lfail

    // ORR/ORN cover LSR and ASR. ORN with ZR complements the shifted value.
    orr x6, xzr, x2, lsr #1
    cmp x6, #2
    b.ne .Lfail
    orn x7, xzr, x2, asr #1
    movn x8, #2
    cmp x7, x8
    b.ne .Lfail

    // EON with an all-ones lhs recovers the shifted operand.
    eon x9, x1, x2, ror #1
    eor x10, x9, x2, ror #1
    cmp x10, #0
    b.ne .Lfail

    // Logical flag-setting forms set N/Z and clear C/V.
    ands xzr, x1, x2, lsl #1
    b.eq .Lfail
    b.mi .Lfail
    ands xzr, x2, xzr
    b.ne .Lfail
    b.cs .Lfail
    b.vs .Lfail
    b.eq .Lzero_flags
    b .Lfail
.Lzero_flags:
    bics xzr, x1, x2
    b.pl .Lfail
    b.eq .Lfail
    b.cs .Lfail
    b.vs .Lfail
    b.mi .Lnegative_flags
    b .Lfail
.Lnegative_flags:
    // W operations truncate their source, result, shifts, and zero-extended writeback.
    orr w12, wzr, w2, lsl #1
    cmp x12, #10
    b.ne .Lfail
    orn w13, wzr, w2, asr #1
    movz x14, #0xfffd
    movk x14, #0xffff, lsl #16
    cmp x13, x14
    b.ne .Lfail
    eor w15, wzr, w2, ror #1
    movz x16, #2
    movk x16, #0x8000, lsl #16
    cmp x15, x16
    b.ne .Lfail
    movn w17, #0
    bics w17, w17, w2
    b.pl .Lfail
    b.cs .Lfail
    b.vs .Lfail

    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
