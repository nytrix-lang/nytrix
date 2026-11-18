/*
 * C front-end regression test: unit function type pointer.
 */
typedef unsigned int hook_fn(void *arg);
extern hook_fn *hook;
hook_fn *hook;

static unsigned int callback(void *arg) { return arg != 0; }

int main(void) {
  hook = callback;
  return hook((void *)0);
}
