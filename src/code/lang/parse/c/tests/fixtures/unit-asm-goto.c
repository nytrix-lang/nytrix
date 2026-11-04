struct nyc_jump_key_holder { int padding; int key; };
static struct nyc_jump_key_holder nyc_jump_key_holder;
static inline int nyc_static_branch(int *key, int branch) {
  asm goto("1: jmp %l[l_yes]\n\t"
           ".pushsection __jump_table, \"aw\"\n\t"
           ".balign 8\n\t.long 1b - .\n\t"
           ".long %l[l_yes] - .\n\t"
           ".quad %c0 + %c1 - .\n\t.popsection\n\t"
           : : "i"(key), "i"(branch) : : l_yes);
  return 0;
l_yes: return 1;
}
int main(void) { return nyc_static_branch(&nyc_jump_key_holder.key, 0) != 1; }
