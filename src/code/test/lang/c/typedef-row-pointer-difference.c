typedef unsigned char row[32];
extern row records[];

__attribute__((noinline)) long forward(void *address, unsigned displacement) {
  row *p = address + displacement;
  return p - records;
}

__attribute__((noinline)) long backward(void *address, unsigned displacement) {
  row *p = address + displacement;
  return records - p;
}

row records[16];

int main(void) {
  unsigned long base = (unsigned long)records;
  return forward(records, 352) != 11 || backward(records, 352) != -11 ||
         (unsigned long)&records[4] - base != 128 ||
         (unsigned long)&records[4][17] - base != 145 ||
         &records[12] - &records[5] != 7;
}
