/*
 * Fixture: emit an LKGS instruction and Linux exception-table metadata.
 */
void nyc_lkgs(unsigned int selector) {
  unsigned short sel = selector;
  __asm__ volatile("1: .byte 0xf2,0x0f,0x00,0xf7 ;\n"
                   " .pushsection __ex_table, \"aM\", @progbits, 12\n"
                   " .balign 4\n .long 1b - .\n .long 1b - .\n"
                   " .macro extable_type_reg type:req reg:req\n"
                   " .long 1809\n .endm\n"
                   " extable_type_reg reg=%%di, type=17\n"
                   " .purgem extable_type_reg\n .popsection\n"
                   : [sel] "+D"(sel));
}
