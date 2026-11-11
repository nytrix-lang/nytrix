/*
 * Address-of a member selected through nested type-based choose expressions.
 */
struct page_table {
  int used;
};

struct page_directory {
  struct page_table pt;
};

struct source {
  struct page_directory *pd;
};

#define NYC_CHOOSE_PT(x)                                                       \
  __builtin_choose_expr(                                                       \
      __builtin_types_compatible_p(typeof(x), struct page_table *) ||          \
          __builtin_types_compatible_p(typeof(x), const struct page_table *),  \
      ({                                                                       \
        struct page_table *__x = (struct page_table *)(x);                     \
        __x;                                                                   \
      }),                                                                      \
      __builtin_choose_expr(                                                   \
          __builtin_types_compatible_p(typeof(x), struct page_directory *) ||  \
              __builtin_types_compatible_p(typeof(x),                          \
                                           const struct page_directory *),     \
          ({                                                                   \
            struct page_directory *__x = (struct page_directory *)(x);         \
            &__x->pt;                                                          \
          }),                                                                  \
          (void)0))

#define NYC_USED(x) (&NYC_CHOOSE_PT(x)->used)

static void increment_used(struct source *source) {
  int *used = NYC_USED(source->pd);
  *used += 1;
}

int main(void) {
  struct page_directory directory = {.pt = {.used = 11}};
  struct source source = {.pd = &directory};
  increment_used(&source);
  return directory.pt.used != 12;
}
