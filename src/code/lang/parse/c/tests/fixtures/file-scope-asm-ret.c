asm(".pushsection .text.nyc_file_asm,\"ax\"\n"
    ".globl nyc_file_asm_ret\n"
    ".type nyc_file_asm_ret,@function\n"
    "nyc_file_asm_ret:\n"
    "ret; int3; nop; nop; nop\n"
    ".size nyc_file_asm_ret, .-nyc_file_asm_ret\n"
    ".popsection\n");
