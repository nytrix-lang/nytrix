.text
.globl __x86_indirect_thunk_r11
.type __x86_indirect_thunk_r11,@function
__x86_indirect_thunk_r11:
 call .Lspeculation_trap
.Lspeculation_loop:
 pause
 lfence
 jmp .Lspeculation_loop
.Lspeculation_trap:
 mov %r11,(%rsp)
 ret
.size __x86_indirect_thunk_r11, .-__x86_indirect_thunk_r11
