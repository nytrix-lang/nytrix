/*
 * Fixture: preserve an asm-goto static-branch target through unit compilation.
 */
struct nyc_jump_key_holder {
  int padding;
  int key;
};
static struct nyc_jump_key_holder nyc_jump_key_holder;
static inline int nyc_static_branch(void) {
  asm goto("1: jmp %l[l_yes]\n\t"
           ".pushsection __jump_table, \"aw\"\n\t"
           ".balign 8\n\t.long 1b - .\n\t"
           ".long %l[l_yes] - .\n\t"
           ".quad %c0 + %c1 - .\n\t.popsection\n\t"
           :
           : "i"(&nyc_jump_key_holder.key), "i"(0)
           :
           : l_yes);
  return 0;
l_yes:
  return 1;
}
static inline int nyc_dynamic_static_branch(int index) {
  asm goto("1: jmp %l[l_yes]\n\t"
           ".pushsection __jump_table, \"aw\"\n\t"
           ".balign 8\n\t.long 1b - .\n\t"
           ".long %l[l_yes] - .\n\t"
           ".quad %c0 + %c1 - .\n\t.popsection\n\t"
           :
           : "i"(&nyc_jump_key_holder.key + index), "i"(0)
           :
           : l_yes);
  return 0;
l_yes:
  return 1;
}
static int nyc_nonconstant_jump_label(int index) {
  if (__builtin_constant_p(index) && nyc_dynamic_static_branch(index))
    return 1;
  return 0;
}
static int nyc_invoke_static_branch(int (*branch)(void)) { return branch(); }
static int (*volatile nyc_branch_table[])(void) = {nyc_static_branch};
int main(void) {
  return nyc_invoke_static_branch(nyc_static_branch) != 1 ||
         nyc_branch_table[0]() != 1 || nyc_nonconstant_jump_label(0) != 0;
}
