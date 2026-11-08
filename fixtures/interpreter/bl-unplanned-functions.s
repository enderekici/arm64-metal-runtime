.text
.globl _bl_unplanned
_bl_unplanned:
    stp x19, x30, [sp, #-16]!
    bl .Lcallee
    nop
    movz w0, #42
    ldp x19, x30, [sp], #16
    ret
.Lcallee:
    nop
    ret
