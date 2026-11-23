/* Stores into a local that only an inline-asm memory operand reads (lgdt) must survive dead-store elimination. */
struct desc_ptr { unsigned short size; unsigned long address; } __attribute__((packed));
static inline void native_load_gdt(const struct desc_ptr *dtr) { asm volatile("lgdt %0" ::"m"(*dtr)); }
extern unsigned long gdt_base(int);
void load_direct_gdt(int cpu) {
  struct desc_ptr gdt_descr;
  gdt_descr.address = gdt_base(cpu);
  gdt_descr.size = 0x7f;
  native_load_gdt(&gdt_descr);
}
