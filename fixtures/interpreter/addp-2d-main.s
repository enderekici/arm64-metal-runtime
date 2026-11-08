.text
.globl _main
_main:
    movi v1.2d, #0xff
    orn v1.16b, v1.16b, v1.16b
    addp d1, v1.2d
    umov x2, v1.d[0]
    movn x3, #1
    cmp x2, x3
    b.ne .Lfail
    umov x2, v1.d[1]
    cbnz x2, .Lfail
    mov x0, #42
    ret
.Lfail:
    mov x0, #1
    ret
