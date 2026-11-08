.text
.p2align 2
.globl _main
_main:
    movz x0, #42
    cmeq v1.8b, v0.8b, #0
    cmeq v2.16b, v0.16b, #0
    cmeq v3.4h, v0.4h, #0
    cmeq v4.8h, v0.8h, #0
    cmeq v5.2s, v0.2s, #0
    cmeq v6.4s, v0.4s, #0
    cmeq v7.2d, v0.2d, #0
    cmeq v0.2d, v0.2d, #0
    ret
