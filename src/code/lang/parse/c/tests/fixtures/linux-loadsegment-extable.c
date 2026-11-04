void nyc_loadsegment_ds(unsigned short value) {
  __asm__ volatile("1: movl %k0,%%ds\n"
                   " .pushsection __ex_table, \"aM\", @progbits, 12\n"
                   " .balign 4\n .long 1b - .\n .long 1b - .\n"
                   " .macro extable_type_reg type:req reg:req\n"
                   " .long 17\n .endm\n"
                   " extable_type_reg reg=%%eax, type=17\n"
                   " .purgem extable_type_reg\n .popsection\n"
                   : "+r"(value)
                   :
                   : "memory");
}
