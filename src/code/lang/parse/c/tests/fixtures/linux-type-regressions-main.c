struct nyc_pair {
  unsigned long left, right;
};
struct nyc_flex {
  unsigned long count;
  unsigned long values[];
};
unsigned long __per_cpu_offset[64];
void nyc_set_pair(struct nyc_pair);
unsigned long nyc_pair_sum(void);
unsigned long nyc_round_up_typeof(unsigned int);
unsigned long nyc_global_array_type(unsigned long);
struct nyc_regs *nyc_typeof_array_plus_zero(void);
unsigned char nyc_pointer_array_index(unsigned long, unsigned long);
int nyc_paravirt_x86_32_call(void);
int nyc_typedef_array_argument(void);
unsigned long nyc_mask_value(unsigned long);
unsigned long nyc_flexible_check(struct nyc_flex *);
int main(void) {
  struct nyc_pair value = {17, 25};
  struct nyc_flex flex = {0};
  nyc_set_pair(value);
  return nyc_pair_sum() != 42 || nyc_round_up_typeof(3) != 8 ||
         nyc_global_array_type(1) != 0 || !nyc_typeof_array_plus_zero() ||
         nyc_pointer_array_index(0, 0) != 0 ||
         nyc_paravirt_x86_32_call() != 1 || nyc_typedef_array_argument() != 1 ||
         nyc_mask_value(0) != 2 || nyc_mask_value(1) != 0 ||
         nyc_mask_value(2) != 32 || nyc_flexible_check(&flex) != 0;
}
