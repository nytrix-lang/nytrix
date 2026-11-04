struct cpuinfo_x86 { unsigned char x86_capability[80]; };
static struct cpuinfo_x86 boot_cpu_data;
static unsigned long nyc_task_size_max(void) {
  unsigned long ret;
  asm volatile("# ALT: oldinstr\n771:\n\tmovq %[small],%0\n772:\n"
    "# ALT: padding\n.skip -(((775f-774f)-(772b-771b)) > 0) * "
    "((775f-774f)-(772b-771b)),0x90\n773:\n"
    ".pushsection .altinstructions, \"aM\", @progbits, 14\n"
    ".long 771b - .\n.long 774f - .\n.4byte (16*32+16)\n"
    ".byte 773b-771b\n.byte 775f-774f\n.popsection\n"
    ".pushsection .altinstr_replacement, \"ax\"\n"
    "# ALT: replacement\n774:\n\tmovq %[large],%0\n775:\n"
    ".popsection\n" : "=r"(ret) : "i"(0),
    [small] "i"(0x7ffffffff000UL), [large] "i"(0xfffffffffff000UL));
  return ret;
}
int main(void) {
  if (nyc_task_size_max() != 0x7ffffffff000UL) return 1;
  boot_cpu_data.x86_capability[66] = 1;
  return nyc_task_size_max() != 0xfffffffffff000UL;
}
