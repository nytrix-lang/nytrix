.text
.globl __put_user_8
__put_user_8:
  movq %rax, (%rcx)
  xorl %ecx, %ecx
  ret
