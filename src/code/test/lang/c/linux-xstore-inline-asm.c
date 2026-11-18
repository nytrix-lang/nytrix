/*
 * C front-end regression test: linux xstore inline asm.
 */
typedef unsigned int u32;

u32 linux_xstore_inline_asm(u32 *address, u32 mode) {
  u32 random_status;
  asm(".byte 0x0F,0xA7,0xC0 /* xstore %%edi (addr=%0) */"
      : "=m"(*address), "=a"(random_status), "+d"(mode), "+D"(address));
  return random_status;
}
