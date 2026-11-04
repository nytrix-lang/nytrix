struct nyc_flex {
  unsigned long count;
  unsigned long values[];
};
struct nyc_pair {
  unsigned long left, right;
};
struct nyc_owner {
  struct nyc_pair pair;
};
struct nyc_regs {
  unsigned long value;
};
#define BITS_TO_LONGS(bits)                                                    \
  (((bits) + (sizeof(long) * 8) - 1) / (sizeof(long) * 8))
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
typedef DECLARE_BITMAP(mce_banks_t, 64);
static struct nyc_owner owner;
static struct nyc_regs regs[2];
typedef unsigned char __u8;
typedef __u8 u8;
enum { N_SMCA_BANK_TYPES = 8 };
static __seg_gs __attribute__((section(
    ".data..percpu"
    "..read_mostly"))) __typeof__(u8[N_SMCA_BANK_TYPES]) smca_bank_counts;
extern unsigned long __per_cpu_offset[64];
void nyc_set_pair(struct nyc_pair value) { owner.pair = value; }
unsigned long nyc_pair_sum(void) { return owner.pair.left + owner.pair.right; }
unsigned long nyc_round_up_typeof(unsigned int sum) {
  return (((sum + sizeof(unsigned int)) - 1) |
          ((__typeof__(sum + sizeof(unsigned int)))(sizeof(unsigned long) -
                                                    1))) +
         1;
}
unsigned long nyc_global_array_type(unsigned long index) {
  _Static_assert(
      __builtin_types_compatible_p(typeof(*(&regs[0])), struct nyc_regs),
      "global array element type");
  return regs[index].value;
}
struct nyc_regs *nyc_typeof_array_plus_zero(void) {
  return (typeof((regs) + 0))regs;
}
unsigned char nyc_pointer_array_index(unsigned long cpu, unsigned long index) {
  return (*({
    do {
      const void *__verify = (typeof((&(smca_bank_counts)) + 0))((void *)0);
      (void)__verify;
    } while (0);
    ({
      unsigned long __ptr;
      __asm__(""
              : "=r"(__ptr)
              : "0"((__typeof_unqual__(*(&smca_bank_counts)) *)((
                  unsigned long)&smca_bank_counts)));
      (typeof((__typeof_unqual__(*(&smca_bank_counts)) *)((
          unsigned long)&smca_bank_counts)))(__ptr + __per_cpu_offset[cpu]);
    });
  }))[index];
}
static unsigned long nyc_paravirt_calls;
static void nyc_paravirt_target(void) { ++nyc_paravirt_calls; }
void nyc_io_outl(unsigned int value, unsigned short port) {
  __asm__ volatile("out"
                   "l"
                   " %"
                   ""
                   "0, %w1"
                   :
                   : "a"(value), "Nd"(port));
}
int nyc_paravirt_x86_32_call(void) {
  unsigned long eax = 0, edx = 0, ecx = 0, stack = 0;
  void (*target)(void) = nyc_paravirt_target;
  __asm__ volatile("# ALT: oldinstr\n"
                   "771:\n\tcall *%[paravirt_opptr]\n772:\n"
                   "# ALT: replacement\n"
                   ".pushsection .altinstructions, \"aM\", @progbits, 14\n"
                   ".popsection\n"
                   : "=a"(eax), "=d"(edx), "=c"(ecx), "+r"(stack)
                   : [paravirt_opptr] "m"(target)
                   : "memory", "cc");
  return nyc_paravirt_calls == 1;
}
static void bitmap_fill(unsigned long *bitmap, unsigned int bits) {
  if (bits == 64)
    bitmap[0] = ~0UL;
}
int nyc_typedef_array_argument(void) {
  if (1) {
    mce_banks_t all_banks;
    bitmap_fill(all_banks, 64);
    return all_banks[0] == ~0UL;
  }
  return 0;
}
unsigned long nyc_mask_value(unsigned long index) {
  static const unsigned long masks[3] = {
      [0] = 1UL << 1,
      [2] = 1UL << 5,
  };
  return masks[index];
}
unsigned long nyc_flexible_check(struct nyc_flex *desc) {
  _Static_assert(!__builtin_types_compatible_p(typeof(desc->values),
                                               typeof(&desc->values[0])),
                 "flexible array must not decay in typeof");
  return sizeof(struct { _Static_assert(1, "complete"); });
}
