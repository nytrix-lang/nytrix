struct large_buffer {
  unsigned char bytes[131072];
};

unsigned long nyc_large_aggregate_size(void) {
  return sizeof(struct large_buffer);
}

int main(void) { return nyc_large_aggregate_size() == 131072 ? 0 : 1; }
