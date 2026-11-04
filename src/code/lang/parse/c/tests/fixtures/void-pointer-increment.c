static unsigned char values[2];

static void *nyc_advance(void *pointer) {
  pointer++;
  return pointer;
}

int main(void) { return nyc_advance(&values[0]) != &values[1]; }
