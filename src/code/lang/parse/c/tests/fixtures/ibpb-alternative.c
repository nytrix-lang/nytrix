/*
 * Fixture: parse the x86 IBPB feature alternative and its fallback.
 */
struct cpuinfo_x86 {
  unsigned char x86_capability[80];
};
static struct cpuinfo_x86 boot_cpu_data;
static int nyc_ibpb_calls;
void write_ibpb(void) { ++nyc_ibpb_calls; }
static void nyc_ibpb_barrier(void) {
  unsigned long current_stack_pointer = 0;
  asm volatile("# ALT: oldinstr\n771:\n772:\n# ALT: padding\n"
               ".skip -(((775f-774f)-(772b-771b)) > 0) * "
               "((775f-774f)-(772b-771b)),0x90\n773:\n"
               ".pushsection .altinstructions, \"aM\", @progbits, 14\n"
               ".long 771b - .\n.long 774f - .\n.4byte ( 7*32+26)\n"
               ".byte 773b-771b\n.byte 775f-774f\n.popsection\n"
               ".pushsection .altinstr_replacement, \"ax\"\n"
               "# ALT: replacement\n774:\n\tcall write_ibpb\n775:\n"
               ".popsection\n"
               : "+r"(current_stack_pointer)
               :
               : "rax", "rcx", "rdx", "memory");
}
int main(void) {
  nyc_ibpb_barrier();
  if (nyc_ibpb_calls != 0)
    return 1;
  boot_cpu_data.x86_capability[31] = 4;
  nyc_ibpb_barrier();
  if (nyc_ibpb_calls != 1)
    return 2;
  boot_cpu_data.x86_capability[31] = 0;
  nyc_ibpb_barrier();
  return nyc_ibpb_calls != 1;
}
