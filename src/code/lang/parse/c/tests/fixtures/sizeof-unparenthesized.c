struct nyc_sizeof_owner { unsigned long long items[40]; };
int main(void) {
  struct nyc_sizeof_owner *owner = 0;
  return sizeof owner->items != 320 ||
         sizeof owner->items[0] != sizeof(unsigned long long);
}
