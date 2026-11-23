/* Nested one-shot `done` loops (Linux scoped user access) run the body once and the cleanup once on every exit. */
int ends;
static void end_access(int *const *pointer) {
  (void)pointer;
  ++ends;
}
static int *begin_access(int *pointer) { return pointer; }
#define SCOPE(pointer)                                                         \
  for (_Bool done = 0; !done; done = 1)                                        \
    for (__auto_type tmp = begin_access(pointer); !done; done = 1)             \
      for (int *const scoped __attribute__((cleanup(end_access))) = tmp;       \
           !done; done = 1)
int scope_normal(int *p) {
  int v = 0;
  SCOPE(p) { v = *p + 1; }
  return v;
}
int scope_return(int *p) {
  int v = 0;
  SCOPE(p) {
    if (*p > 2)
      return -14;
    v = 1;
  }
  return v;
}
int scope_goto(int *p) {
  int v = 0;
  SCOPE(p) {
    if (*p > 2)
      goto efault;
    v = 1;
  }
  return v;
efault:
  return -14;
}
int scope_break(int *p) {
  int v = 0;
  SCOPE(p) {
    v = 1;
    if (*p > 2)
      break;
    v = 2;
  }
  return v;
}
int scope_continue(int *p) {
  int v = 0;
  SCOPE(p) {
    v = 1;
    if (*p > 2)
      continue;
    v = 2;
  }
  return v;
}
