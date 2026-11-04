#define NYC_VA(first, ...) first, ##__VA_ARGS__
#define NYC_STRINGIFY_1(x...) #x
#define NYC_STRINGIFY(x...) NYC_STRINGIFY_1(x)
#define NYC_SUFFIX(x, ...) x##__VA_ARGS__##_ctx_lock
#define nyc_pasted_shared_ctx_lock 77
#define NYC_CAT_I(a, b) a##b
#define NYC_CAT(a, b) NYC_CAT_I(a, b)
#define NYC_COUNT_I(_1, _2, N, ...) N
#define NYC_COUNT(...) NYC_COUNT_I(__VA_ARGS__, 2, 1)
#define NYC_PICK_I(n) NYC_CAT(NYC_PICK_, n)
#define NYC_PICK(...) NYC_PICK_I(NYC_COUNT(__VA_ARGS__))(__VA_ARGS__)
#define NYC_PICK_2(a, b) b
#define NYC_PATH trace / events
#define NYC_TRACE_INCLUDE(system) NYC_STRINGIFY(NYC_PATH / system.h)
#include NYC_TRACE_INCLUDE(variadic)
int omitted[] = {NYC_VA(1)};
int provided[] = {NYC_VA(1, 2)};
int pasted = NYC_SUFFIX(nyc_pasted, _shared);
int nested_dispatch = NYC_PICK(9, 77);
const char *stringified = NYC_STRINGIFY(1, 2);
#if NYC_TRACE_PATH_INCLUDED
int trace_path_found;
#endif
#define NYC_CONTINUED(value)                                                   \
  value /* Preserve a comment                                                  \
         * across continued physical lines                                     \
         */                                                                    \
      + 1
int comment_macro = NYC_CONTINUED(40);

int main(void) {
  const char expected[] = "1, 2";
  if (sizeof(omitted) != sizeof(int) || omitted[0] != 1 ||
      sizeof(provided) != 2 * sizeof(int) || provided[0] != 1 ||
      provided[1] != 2 || pasted != 77 || nested_dispatch != 77 ||
      comment_macro != 41 || trace_path_found != 0)
    return 1;
  for (unsigned i = 0; i < sizeof(expected); ++i)
    if (stringified[i] != expected[i])
      return 2;
  return 0;
}
