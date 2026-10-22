void c_void_store(long *out, long value) { *out = value; }

void c_void_store_return(long *out, long value) {
  *out = value;
  return;
}

long c_void_call(long *out, long value) {
  c_void_store_return(out, value);
  return *out;
}

void c_void_return_value(void) { return 1; }
