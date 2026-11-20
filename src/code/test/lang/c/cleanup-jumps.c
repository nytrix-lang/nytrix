static int cleaned;
static int jump_key;

static void record(int *value) { cleaned = cleaned * 10 + *value; }

static int nested_goto(int enter) {
  int outer __attribute__((cleanup(record))) = 1;
  for (int loop __attribute__((cleanup(record))) = 2; enter;) {
    int inner __attribute__((cleanup(record))) = 3;
    goto done;
  }
done:
  return cleaned;
}

static int asm_branch(void) {
  int outer __attribute__((cleanup(record))) = 4;
  {
    int inner __attribute__((cleanup(record))) = 5;
    asm goto("1: jmp %l[l_yes]\n\t"
             ".pushsection __jump_table, \"aw\"\n\t"
             ".balign 8\n\t.long 1b - .\n\t"
             ".long %l[l_yes] - .\n\t"
             ".quad %c0 + %c1 - .\n\t.popsection\n\t"
             :
             : "i"(&jump_key), "i"(0)
             :
             : l_yes);
    return -1;
  }
l_yes:
  return cleaned;
}

#define LOOP_EXIT(n) scope_exit_##n
#define LOOP_LOCAL int guard __attribute__((cleanup(record)))
#define SCOPED_LOOP(n)                                                         \
  for (LOOP_LOCAL = 6; guard; ({ goto LOOP_EXIT(n); }))                        \
    if (0) {                                                                   \
      LOOP_EXIT(n) : break;                                                    \
    } else

static int loop_step_goto(void) {
  SCOPED_LOOP(1) {
    int body __attribute__((cleanup(record))) = 7;
    cleaned = 8;
  }
  return cleaned;
}

static int macro_label_backedge(int iterations) {
  LOOP_EXIT(2) :;
  LOOP_LOCAL = iterations;
  if (--iterations)
    goto LOOP_EXIT(2);
  return cleaned;
}

int main(void) {
  int reached = nested_goto(1);
  if (reached != 32 || cleaned != 321)
    return 1;
  cleaned = 0;
  reached = asm_branch();
  if (reached != 5 || cleaned != 54)
    return 2;
  cleaned = 0;
  reached = loop_step_goto();
  if (reached != 876 || cleaned != 876)
    return 3;
  cleaned = 0;
  reached = macro_label_backedge(2);
  return reached != 2 || cleaned != 21;
}
