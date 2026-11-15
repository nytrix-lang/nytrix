typedef unsigned long long trace_address_t;

static const void *pm_trace_metadata(void) {
  const void *tracedata;
  asm volatile("movq $1f,%0\n"
               ".section .tracedata,\"a\"\n"
               "1:\t.word %c1\n\t"
               ".quad %c2\n"
               ".previous"
               : "=r"(tracedata)
               : "i"(__LINE__), "i"(__FILE__));
  return tracedata;
}

int main(void) {
  const unsigned char *data = pm_trace_metadata();
  if (!data || (data[0] == 0 && data[1] == 0))
    return 1;
  trace_address_t file_address = 0;
  for (unsigned long i = 0; i < sizeof(file_address); ++i)
    file_address |= (trace_address_t)data[2 + i] << (i * 8);
  const char *file = (const char *)file_address;
  const char *expected = __FILE__;
  for (unsigned long i = 0; expected[i] || file[i]; ++i)
    if (expected[i] != file[i])
      return 2;
  return 0;
}
