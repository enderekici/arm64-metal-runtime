.text
.globl _main
_main:
    bl _bl_nested
    subs xzr, x0, #42
    b.ne .Lfail
    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
