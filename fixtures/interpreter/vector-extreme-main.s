.text
.p2align 2
.globl _main
_main:
    movz x0, #42
    umaxv b1, v0.8b
    umaxv b2, v0.16b
    umaxv h3, v0.4h
    umaxv h4, v0.8h
    umaxv s5, v0.4s
    uminv b6, v0.8b
    uminv b7, v0.16b
    uminv h8, v0.4h
    uminv h9, v0.8h
    uminv s10, v0.4s
    smaxv b11, v0.8b
    smaxv b12, v0.16b
    smaxv h13, v0.4h
    smaxv h14, v0.8h
    smaxv s15, v0.4s
    sminv b16, v0.8b
    sminv b17, v0.16b
    sminv h18, v0.4h
    sminv h19, v0.8h
    sminv s20, v0.4s
    umaxv s0, v0.4s
    ret
