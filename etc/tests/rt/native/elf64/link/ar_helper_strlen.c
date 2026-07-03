extern unsigned long strlen(const char *s);
int ar_strlen_hello(void) {
  const char *msg = "hello there";
  return (int)strlen(msg) + 42;
}
