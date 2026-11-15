/*
 * C parser fixture for function-pointer arrays, declarations, and sections.
 */
typedef void (*nyc_initcall_entry_t)(void);
extern nyc_initcall_entry_t nyc_initcall_start[], nyc_initcall_end[];
extern long (*nyc_panic_blink)(int state);
long (*__attribute__((section(".nyc_"
                              "fnptr"))) nyc_section_blink)(int state);
static const char *nyc_fixed_args[3] = {
    "one",
    NULL,
};
static const char *nyc_decl_a, *nyc_decl_b;
struct nyc_self_node {
  struct nyc_self_node *next;
};
static struct nyc_self_node nyc_self = {.next = &nyc_self};
struct nyc_kernel_param {
  const char *name;
  void *mod;
  const void *ops;
  unsigned perm;
  int level;
  unsigned flags;
  union {
    void *arg;
  };
};
static const int nyc_param_value;
static const int nyc_param_ops;
static const char nyc_param_str[] = "initcall_debug";
static struct nyc_kernel_param const nyc_param __attribute__((used))
__attribute__((section("__param")))
__attribute__((aligned(__alignof__(struct nyc_kernel_param)))) = {
    nyc_param_str,     ((void *)0), &nyc_param_ops, 0644, -1, 0,
    {&nyc_param_value}};
_Static_assert(sizeof(struct { _Static_assert(1, "empty type"); }) == 0,
               "GNU empty struct size for assertion-only members");
int nyc_initcall_ranges_equal(void) {
  return nyc_initcall_start == nyc_initcall_end;
}
int nyc_const_filter(int (*const filter)(void)) { return filter != 0; }
typedef union {
  void *pointer;
  long integer;
} nyc_transparent_t __attribute__((__transparent_union__));
void nyc_take_transparent(nyc_transparent_t value);
#define NYC_PASS(x) x
#define NYC_FEATURE 1
struct nyc_conditional_macro_arg {
  NYC_PASS(
#ifdef NYC_FEATURE
      int enabled;
#else
      int disabled;
#endif
  )
};
_Static_assert(sizeof(struct nyc_conditional_macro_arg) == 4,
               "conditionals inside macro arguments");
struct nyc_compound_value {
  int value;
};
static struct nyc_compound_value nyc_compound = (struct nyc_compound_value){7};
typedef int nyc_initcall_marker_t;
extern nyc_initcall_marker_t nyc_initcall0_start[];
static nyc_initcall_marker_t *nyc_initcall_levels[] = {nyc_initcall0_start, 0};
static int nyc_direct_init(void) { return 0; }
static int (*nyc_direct_initcalls[])(void)
    __attribute__((section(".init.data"))) = {nyc_direct_init};
typedef void(__attribute__((ms_abi)) * nyc_efi_event_notify_t)(void *, void *);
nyc_efi_event_notify_t nyc_efi_event_notify;
typedef void *nyc_event_t;
void nyc_set_event_at(nyc_event_t *events, unsigned long index,
                      nyc_event_t event) {
  events[index] = event;
}
static const char *nyc_initcall_level_names[]
    __attribute__((section(".init.data"))) = {"pure", "core"};
