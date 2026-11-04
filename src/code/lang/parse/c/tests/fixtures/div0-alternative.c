struct cpuinfo_x86 { unsigned char x86_capability[96]; };
static struct cpuinfo_x86 boot_cpu_data;
static void amd_clear_divider(void) {
  asm volatile("# ALT: oldinstr\n771:\n772:\n# ALT: padding\n"
    ".skip -(((775f-774f)-(772b-771b)) > 0) * "
    "((775f-774f)-(772b-771b)),0x90\n773:\n"
    ".pushsection .altinstructions, \"aM\", @progbits, 14\n"
    ".long 771b - .\n.long 774f - .\n"
    ".4byte (22*32 + (1*32+ 1))\n"
    ".byte 773b-771b\n.byte 775f-774f\n.popsection\n"
    ".pushsection .altinstr_replacement, \"ax\"\n"
    "# ALT: replacement\n774:\n\tdiv %2\n\t\n775:\n"
    ".popsection\n" :: "a"(0), "d"(0), "r"(1));
}
int main(void) {
  amd_clear_divider();
  boot_cpu_data.x86_capability[92] = 2;
  amd_clear_divider();
  boot_cpu_data.x86_capability[92] = 0;
  amd_clear_divider();
  return 0;
}
