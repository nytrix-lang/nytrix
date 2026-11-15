/*
 * Linker-generated bounds verify placement, sizes, and relocated values.
 */
struct state {
  int value;
};
struct note {
  unsigned namesz, descsz, type;
  char name[4];
  unsigned description;
};
static const struct note
    __attribute__((section(".note.nyc"), aligned(4), used)) nyc_note = {
        4, 4, 0x4e59, "NYC", 0x12345678};
struct state __attribute__((section("nyc_agg"))) a = {11};
struct state __attribute__((section("nyc_agg_array"))) aa[2] = {{13}, {17}};
int __attribute__((section("nyc_int_array"))) ia[2] = {19, 23};
struct state *__attribute__((section("nyc_ptr_array"))) pa[1] = {&a};
static int __attribute__((section("nyc_code"))) foo(void) { return 29; }
typedef int (*callback)(void);
callback __attribute__((section("nyc_callback"))) cb = foo;
int __attribute__((section("nyc_callback_direct"))) (*cb2)(void) = foo;
extern struct state __start_nyc_agg[], __stop_nyc_agg[];
extern struct state __start_nyc_agg_array[], __stop_nyc_agg_array[];
extern int __start_nyc_int_array[], __stop_nyc_int_array[];
extern struct state *__start_nyc_ptr_array[], *__stop_nyc_ptr_array[];
extern callback __start_nyc_callback[], __stop_nyc_callback[];
extern callback __start_nyc_callback_direct[], __stop_nyc_callback_direct[];
extern char __start_nyc_code[], __stop_nyc_code[];
int main(void) {
  if (nyc_note.namesz != 4 || nyc_note.descsz != 4 ||
      nyc_note.description != 0x12345678)
    return 8;
  if (&__start_nyc_agg[0] != &a || &__stop_nyc_agg[0] != &a + 1 ||
      a.value != 11)
    return 1;
  if (&__start_nyc_agg_array[0] != aa || &__stop_nyc_agg_array[0] != aa + 2 ||
      aa[1].value != 17)
    return 2;
  if (&__start_nyc_int_array[0] != ia || &__stop_nyc_int_array[0] != ia + 2 ||
      ia[1] != 23)
    return 3;
  if (&__start_nyc_ptr_array[0] != pa || &__stop_nyc_ptr_array[0] != pa + 1 ||
      pa[0] != &a)
    return 4;
  if (&__start_nyc_callback[0] != &cb || &__stop_nyc_callback[0] != &cb + 1 ||
      cb() != 29)
    return 5;
  if (&__start_nyc_callback_direct[0] != &cb2 ||
      &__stop_nyc_callback_direct[0] != &cb2 + 1 || cb2() != 29)
    return 6;
  if ((void *)&__start_nyc_code[0] != (void *)foo ||
      (void *)&__stop_nyc_code[0] <= (void *)foo)
    return 7;
  return 0;
}
