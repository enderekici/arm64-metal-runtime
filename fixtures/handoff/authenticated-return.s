.text
.globl _auth_key_b_positive
.p2align 2
_auth_key_b_positive:
    pacibsp
    mov x0, xzr
    autibsp
    ret

.globl _auth_key_b_retab_positive
.p2align 2
_auth_key_b_retab_positive:
    pacibsp
    mov x0, xzr
    retab

.globl _auth_key_b_bad_lr
.p2align 2
_auth_key_b_bad_lr:
    pacibsp
    mov x30, xzr
    autibsp
    ret

.globl _auth_key_b_bad_sp
.p2align 2
_auth_key_b_bad_sp:
    pacibsp
    sub sp, sp, #16
    autibsp
    add sp, sp, #16
    ret

.globl _auth_key_b_bad_lr_retab
.p2align 2
_auth_key_b_bad_lr_retab:
    pacibsp
    mov x30, xzr
    retab

.globl _auth_key_b_bad_sp_retab
.p2align 2
_auth_key_b_bad_sp_retab:
    pacibsp
    sub sp, sp, #16
    retab
