/*
 * File-scope assembly fixture that defines a callable text symbol.
 */
asm(".pushsection .text.nyc_file_asm,\"ax\"\n"
    ".globl nyc_file_asm_ret\n"
    ".type nyc_file_asm_ret,@function\n"
    "nyc_file_asm_ret:\n"
    "pushq %r8\n"
    "subq $8, %rsp\n"
    "addq $8, %rsp\n"
    "popq %r8\n"
    "ret; int3; nop; nop; nop\n"
    ".size nyc_file_asm_ret, .-nyc_file_asm_ret\n"
    ".popsection\n");
