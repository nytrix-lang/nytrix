/*
 * C front-end regression test: array member designator.
 */
typedef struct {
  unsigned int reg;
} i915_reg_t;

typedef struct {
  unsigned int reg;
  unsigned int addr;
} i915_mcr_reg_t;

typedef union {
  i915_reg_t reg;
  i915_mcr_reg_t mcr_reg;
} inv_reg_t;

int main(void) {
  static const inv_reg_t regs[] = {
      [0].reg = ((const i915_reg_t){.reg = 0x4260}),
      [2].reg = ((const i915_reg_t){.reg = 0x4264}),
  };
  return regs[0].reg.reg + regs[2].reg.reg != 0x84c4 || regs[1].reg.reg != 0;
}
