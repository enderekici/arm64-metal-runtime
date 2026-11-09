.text
.globl _main
_main:
    fmov d8, #0.75
    fmov d0, #-1.0
    fmov d0, d8
    fcmp d0, d8
    b.ne failed
    mov w0, #42
    ret
failed:
    mov w0, #1
    ret
