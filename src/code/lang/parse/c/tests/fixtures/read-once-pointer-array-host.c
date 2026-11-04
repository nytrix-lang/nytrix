struct nyc_read_once_item { int value; };
struct nyc_read_once_item nyc_read_once_expected = {42};
struct nyc_read_once_item *nyc_read_once_items[] = {0, &nyc_read_once_expected};
int nyc_read_once_pointer_array_ok(void);
int main(void) { return nyc_read_once_pointer_array_ok() ? 0 : 1; }
