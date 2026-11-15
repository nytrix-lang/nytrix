.text
.globl __get_user_8
__get_user_8:
  movabsq $0x1234567800000003, %rax
  movq $5, %rdx
  ret
