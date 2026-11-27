/*
 * Host driver for i386-pointer-member-store.c.
 */
struct card {
  const char *name;
  int (*set)(void *);
  void *modes;
  int count;
};
extern void nyc_set_name(struct card *, const char *);
extern void nyc_set_modes(struct card *, void *);

static int set_hook(void *p) { return p != 0; }

static int run(void) {
  struct card card = {0, set_hook, 0, 77};
  nyc_set_name(&card, "vga");
  nyc_set_modes(&card, &card);
  if (card.set != set_hook || card.count != 77 || card.modes != &card ||
      card.name == 0 || card.name[0] != 'v')
    return 1;
  return 0;
}

__attribute__((force_align_arg_pointer)) void _start(void) {
  int status = run();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
