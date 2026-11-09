.text
.globl _main
_main:
    mov x0, #0
    .globl _vector_memory_fault_load
_vector_memory_fault_load:
    ldr q1, [x0]
    mov w0, #42
    ret
