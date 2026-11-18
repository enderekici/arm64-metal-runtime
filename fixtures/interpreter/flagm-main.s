.text
.p2align 2
.globl _main

// Check an architectural NZCV nibble without changing any flags.
.macro expect_nzcv value
    .if (\value & 8)
        b.pl Lfail
    .else
        b.mi Lfail
    .endif
    .if (\value & 4)
        b.ne Lfail
    .else
        b.eq Lfail
    .endif
    .if (\value & 2)
        b.cc Lfail
    .else
        b.cs Lfail
    .endif
    .if (\value & 1)
        b.vc Lfail
    .else
        b.vs Lfail
    .endif
.endm

_main:
    cmp xzr, xzr
    mov x8, #-1
    rmif x8, #0, #0
    expect_nzcv 6
    rmif xzr, #63, #15
    expect_nzcv 0
    rmif x8, #0, #8
    expect_nzcv 8
    rmif x8, #0, #4
    expect_nzcv 12
    rmif x8, #0, #2
    expect_nzcv 14
    rmif x8, #0, #1
    expect_nzcv 15

    // Rotation by 63 takes the new bit 0 from bit 63.
    mov x8, #6
    movk x8, #0x8000, lsl #48
    rmif x8, #63, #15
    expect_nzcv 13
    mov x8, #0x20
    rmif x8, #4, #7
    expect_nzcv 10

    setf8 wzr
    expect_nzcv 6
    mov w8, #0x180
    setf8 w8
    expect_nzcv 10
    mov w8, #0x100
    setf8 w8
    expect_nzcv 7
    mov w8, #0x7f
    movk w8, #0xffff, lsl #16
    setf8 w8
    expect_nzcv 2

    rmif xzr, #0, #2
    expect_nzcv 0
    mov w8, #0x80
    setf8 w8
    expect_nzcv 9
    mov w8, #0x8000
    setf16 w8
    expect_nzcv 9
    mov w8, #0x10000
    setf16 w8
    expect_nzcv 5
    mov w8, #0x18000
    setf16 w8
    expect_nzcv 8
    mov w8, #0x7f01
    setf16 w8
    expect_nzcv 0
    cmp xzr, xzr
    setf16 wzr
    expect_nzcv 6
    mov x0, #42
    ret
Lfail:
    mov x0, #1
    ret
