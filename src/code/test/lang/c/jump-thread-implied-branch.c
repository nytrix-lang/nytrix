/* `if (ok) enter(); if (!ok) goto fail;` : the second test is decided by the
   first on every path, so the entered path must not reach the failure exit. */
extern int check(long);
extern int work(void);
int implied(long p) {
  int err = -14;
  _Bool ok = check(p);
  if (ok)
    __asm__ volatile("nop" ::: "memory");
  if (!ok)
    goto failed;
  err = work();
  __asm__ volatile("nop" ::: "memory");
failed:
  return err;
}
