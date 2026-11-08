.text
.p2align 2
.globl _main
.set bcond_check, 99
.macro EXPECT_TAKEN condition
    .set bcond_check, bcond_check + 1
    b.\condition 1f
    movz w0, #bcond_check
    ret
1:
.endmacro

.macro EXPECT_NOT_TAKEN condition
    .set bcond_check, bcond_check + 1
    b.\condition 1f
    b 2f
1:
    movz w0, #bcond_check
    ret
2:
.endmacro

_main:
    movz w10, #0
    subs w11, w10, #0
    EXPECT_TAKEN eq
    EXPECT_NOT_TAKEN ne
    EXPECT_TAKEN cs
    EXPECT_NOT_TAKEN cc
    EXPECT_NOT_TAKEN mi
    EXPECT_TAKEN pl
    EXPECT_NOT_TAKEN vs
    EXPECT_TAKEN vc
    EXPECT_NOT_TAKEN hi
    EXPECT_TAKEN ls
    EXPECT_TAKEN ge
    EXPECT_NOT_TAKEN lt
    EXPECT_NOT_TAKEN gt
    EXPECT_TAKEN le
    EXPECT_TAKEN al
    EXPECT_TAKEN nv

    movz x10, #0
    subs x11, x10, #1
    EXPECT_NOT_TAKEN eq
    EXPECT_TAKEN ne
    EXPECT_NOT_TAKEN cs
    EXPECT_TAKEN cc
    EXPECT_TAKEN mi
    EXPECT_NOT_TAKEN pl
    EXPECT_NOT_TAKEN vs
    EXPECT_TAKEN vc
    EXPECT_NOT_TAKEN hi
    EXPECT_TAKEN ls
    EXPECT_NOT_TAKEN ge
    EXPECT_TAKEN lt
    EXPECT_NOT_TAKEN gt
    EXPECT_TAKEN le
    EXPECT_TAKEN al
    EXPECT_TAKEN nv

    movz w10, #0xffff
    movk w10, #0x7fff, lsl #16
    adds w11, w10, #1
    EXPECT_NOT_TAKEN eq
    EXPECT_TAKEN ne
    EXPECT_NOT_TAKEN cs
    EXPECT_TAKEN cc
    EXPECT_TAKEN mi
    EXPECT_NOT_TAKEN pl
    EXPECT_TAKEN vs
    EXPECT_NOT_TAKEN vc
    EXPECT_NOT_TAKEN hi
    EXPECT_TAKEN ls
    EXPECT_TAKEN ge
    EXPECT_NOT_TAKEN lt
    EXPECT_TAKEN gt
    EXPECT_NOT_TAKEN le
    EXPECT_TAKEN al
    EXPECT_TAKEN nv

    movz x10, #5
    subs x11, x10, #1
    EXPECT_NOT_TAKEN eq
    EXPECT_TAKEN ne
    EXPECT_TAKEN cs
    EXPECT_NOT_TAKEN cc
    EXPECT_NOT_TAKEN mi
    EXPECT_TAKEN pl
    EXPECT_NOT_TAKEN vs
    EXPECT_TAKEN vc
    EXPECT_TAKEN hi
    EXPECT_NOT_TAKEN ls
    EXPECT_TAKEN ge
    EXPECT_NOT_TAKEN lt
    EXPECT_TAKEN gt
    EXPECT_NOT_TAKEN le
    EXPECT_TAKEN al
    EXPECT_TAKEN nv

    movz w10, #0
    adds w11, w10, #1
    EXPECT_NOT_TAKEN eq
    EXPECT_TAKEN ne
    EXPECT_NOT_TAKEN cs
    EXPECT_TAKEN cc
    EXPECT_NOT_TAKEN mi
    EXPECT_TAKEN pl
    EXPECT_NOT_TAKEN vs
    EXPECT_TAKEN vc
    EXPECT_NOT_TAKEN hi
    EXPECT_TAKEN ls
    EXPECT_TAKEN ge
    EXPECT_NOT_TAKEN lt
    EXPECT_TAKEN gt
    EXPECT_NOT_TAKEN le
    EXPECT_TAKEN al
    EXPECT_TAKEN nv

    movz w0, #42
    ret
