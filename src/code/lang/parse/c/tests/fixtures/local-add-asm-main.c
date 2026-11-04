struct nyc_local_counter {
  struct { long counter; } a;
};
void nyc_local_add_probe(long, struct nyc_local_counter *);
void nyc_local_sub_probe(long, struct nyc_local_counter *);
int main(void) {
  struct nyc_local_counter value = {{7}};
  nyc_local_add_probe(5, &value);
  if (value.a.counter != 12)
    return 1;
  nyc_local_add_probe(-3, &value);
  if (value.a.counter != 9)
    return 2;
  nyc_local_sub_probe(4, &value);
  return value.a.counter != 5;
}
