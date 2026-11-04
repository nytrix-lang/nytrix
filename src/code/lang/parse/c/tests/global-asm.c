__asm__(".section \"nyc_asm_probe\", \"a\"\n"
        ".balign 4\n"
        ".long nyc_asm_target - .\n"
        ".previous\n");

int nyc_asm_target;
int nyc_asm_function(void) { return 42; }
static void nyc_asm_static_scrub(void) {}
int nyc_asm_dummy(void) { return nyc_asm_target + nyc_asm_function(); }

/*
 * Keep the returned stack pointer intact: reversing AT&T operands here makes
 * RET use the returned value as its stack and reliably breaks the caller.
 */
__asm__(".text\n"
        ".globl nyc_asm_read_rsp\n"
        "nyc_asm_read_rsp:\n"
        "movq %rsp, %rax\n"
        "ret\n"
        ".size nyc_asm_read_rsp, . - nyc_asm_read_rsp\n");

__asm__(".section .nyc_asm_static_fixup, \"a\"\n"
        ".long nyc_asm_static_scrub - .\n"
        ".previous\n");

int __tracepoint_nyc_asm_tracepoint;
#define NYC_TRACEPOINT_ENTRY(name)                                             \
  asm(".section \"__tracepoints_ptrs\", \"a\"\n"                               \
      ".balign 4\n"                                                            \
      ".globl nyc_tracepoint_ptr_entry\n"                                      \
      "nyc_tracepoint_ptr_entry:\n"                                            \
      ".long __tracepoint_" #name " - .\n"                                     \
      ".previous\n")
NYC_TRACEPOINT_ENTRY(nyc_asm_tracepoint);

extern int nyc_asm_trampoline(void);
__asm__(".pushsection .static_call.text, \"ax\"\n"
        ".align 4\n"
        ".globl nyc_asm_trampoline\n"
        "nyc_asm_trampoline:\n"
        "911:\n"
        ".pushsection .discard.annotate_insn,\"M\",@progbits,8; "
        ".long 911b - .,1; .popsection\n"
        ".byte 0xe9\n"
        ".long nyc_asm_function - (. + 4)\n"
        ".byte 0x90\n"
        ".type nyc_asm_trampoline, @function\n"
        ".size nyc_asm_trampoline, . - nyc_asm_trampoline\n"
        ".popsection\n");

__asm__(".section .nyc_asm_abs, \"a\"\n"
        ".globl nyc_asm_abs\n"
        "nyc_asm_abs:\n"
        ".asciz \"A\"\n"
        ".ascii \"B\\0\"\n"
        ".balign 8\n"
        ".quad nyc_asm_target\n"
        ".long (~0)\n"
        ".previous\n");

__asm__(".section .nyc_asm_strings, \"a\"\n"
        ".globl nyc_asm_strings\n"
        "nyc_asm_strings:\n"
        ".ascii \"module:\" \"autofs4\" \"\\0\"\n"
        ".previous\n");

__asm__(".pushsection .discard.ibt_endbr_noseal\n"
        ".popsection\n"
        ".pushsection .text.nyc_asm_bare\n"
        ".balign 4\n"
        ".globl nyc_asm_bare_trampoline\n"
        "nyc_asm_bare_trampoline:\n"
        "endbr64\n"
        "jmp nyc_asm_function\n"
        ".type nyc_asm_bare_trampoline, @function\n"
        ".size nyc_asm_bare_trampoline, . - nyc_asm_bare_trampoline\n"
        ".popsection\n");

__asm__(".text\n"
        ".global nyc_asm_text_trampoline\n"
        "nyc_asm_text_trampoline:\n"
        "ret\n"
        ".balign 4, 0x90\n"
        ".skip 2, 0x90\n"
        ".size nyc_asm_text_trampoline, . - nyc_asm_text_trampoline\n");
