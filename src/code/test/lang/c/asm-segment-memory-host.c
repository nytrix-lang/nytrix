/*
 * Host-side C counterpart for the asm segment memory test.
 */
extern int run_segment_test(void);
__asm__(".text\n.globl run_segment_test\n.type run_segment_test,@function\n"
        "run_segment_test:\n"
        "\tpushq %r12\n\tsubq $48,%rsp\n"
        "\txorl %eax,%eax\n\tmovw %fs,%ax\n\tmovw %ax,16(%rsp)\n"
        "\tmovw %gs,%ax\n\tmovw %ax,18(%rsp)\n"
        "\tmovl $90,%r12d\n"
        "\tmovl $158,%eax\n\tmovl $0x1003,%edi\n\tleaq 0(%rsp),%rsi\n"
        "\tsyscall\n\ttestq %rax,%rax\n\tjnz 2f\n"
        "\tmovl $158,%eax\n\tmovl $0x1004,%edi\n\tleaq 8(%rsp),%rsi\n"
        "\tsyscall\n\ttestq %rax,%rax\n\tjnz 2f\n"
        "\tmovl $91,%r12d\n\txorl %edi,%edi\n\txorl %esi,%esi\n"
        "\tcall configure_segments\n\ttestl %eax,%eax\n\tjnz 1f\n"
        "\tmovl $92,%r12d\n"
        "\tmovl $158,%eax\n\tmovl $0x1002,%edi\n\tleaq fs_memory(%rip),%rsi\n"
        "\tsyscall\n\ttestq %rax,%rax\n\tjnz 1f\n"
        "\tmovl $158,%eax\n\tmovl $0x1001,%edi\n\tleaq gs_memory(%rip),%rsi\n"
        "\tsyscall\n\ttestq %rax,%rax\n\tjnz 1f\n"
        "\tcall exercise_segments\n\tmovl %eax,%r12d\n"
        "1:\n\tmovw 16(%rsp),%ax\n\tmovw %ax,%fs\n"
        "\tmovw 18(%rsp),%ax\n\tmovw %ax,%gs\n"
        "\tmovl $158,%eax\n\tmovl $0x1002,%edi\n\tmovq 0(%rsp),%rsi\n"
        "\tsyscall\n"
        "\tmovl $158,%eax\n\tmovl $0x1001,%edi\n\tmovq 8(%rsp),%rsi\n"
        "\tsyscall\n"
        "2:\n\tmovl %r12d,%eax\n\taddq $48,%rsp\n\tpopq %r12\n\tret\n"
        ".size run_segment_test,.-run_segment_test\n");

int main(void) { return run_segment_test(); }
