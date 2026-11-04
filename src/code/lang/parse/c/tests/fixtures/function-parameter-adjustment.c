struct nyc_item { int value; };
static int nyc_seen;
static void nyc_record_item(struct nyc_item *item) {
  nyc_seen = item->value;
}
static void nyc_invoke(void (callback)(struct nyc_item *),
                       struct nyc_item *item) {
  callback(item);
}
int main(void) {
  struct nyc_item item = {7};
  nyc_invoke(nyc_record_item, &item);
  return nyc_seen != 7;
}
