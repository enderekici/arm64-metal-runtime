.text
.globl _bl_nested
_bl_nested:
    stp x19, x30, [sp, #-16]!
    movz x0, #7
    bl .Louter
    ldp x19, x30, [sp], #16
    ret
.Louter:
    stp x20, x30, [sp, #-16]!
    bl .Linner
    ldp x20, x30, [sp], #16
    ret
.Linner:
    add x0, x0, #35
    ret
