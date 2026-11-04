struct nyc_typeof_ops { int (*call)(int, int); };
extern struct nyc_typeof_ops nyc_typeof_ops;
extern typeof(*nyc_typeof_ops.call) nyc_typeof_trampoline;
int nyc_typeof_function_designator_call(int left, int right) {
  return ({ (void)&nyc_typeof_ops; (&nyc_typeof_trampoline); })(left, right);
}
