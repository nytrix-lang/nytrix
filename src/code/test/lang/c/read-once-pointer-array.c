/*
 * C fixture for read-once volatile access through an indexed pointer array.
 */
struct nyc_read_once_item {
  int value;
};
extern struct nyc_read_once_item *nyc_read_once_items[];
static struct nyc_read_once_item *nyc_read_once(int index) {
  return ({
    (*(const volatile typeof(_Generic((nyc_read_once_items[index]),
                                 char: (char)0,
                                 unsigned char: (unsigned char)0,
                                 signed char: (signed char)0,
                                 unsigned short: (unsigned short)0,
                                 signed short: (signed short)0,
                                 unsigned int: (unsigned int)0,
                                 signed int: (int)0,
                                 unsigned long: (long)0,
                                 signed long: (long)0,
                                 unsigned long long: (long long)0,
                                 signed long long: (long long)0,
                                 default: (nyc_read_once_items[index])))
           *)&(nyc_read_once_items[index]));
  });
}
int nyc_read_once_pointer_array_ok(void) {
  return nyc_read_once(1)->value == 42;
}
