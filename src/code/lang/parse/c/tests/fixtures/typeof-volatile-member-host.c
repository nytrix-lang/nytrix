void nyc_typeof_volatile_member_store(unsigned char value);
unsigned char nyc_typeof_volatile_member_value(void);
int main(void) {
  nyc_typeof_volatile_member_store(37);
  if (nyc_typeof_volatile_member_value() != 37) return 1;
  nyc_typeof_volatile_member_store(0);
  return nyc_typeof_volatile_member_value() != 0;
}
