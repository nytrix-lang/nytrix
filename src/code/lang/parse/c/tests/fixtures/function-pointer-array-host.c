typedef void (*nyc_initcall_entry_t)(void);
nyc_initcall_entry_t nyc_initcall_start[1], nyc_initcall_end[1];
int nyc_initcall0_start[1];
int nyc_initcall_ranges_equal(void);
void nyc_set_event_at(void **events, unsigned long index, void *event);
int main(void) {
  void *events[1] = {0};
  nyc_set_event_at(events, 0, (void *)42);
  return nyc_initcall_ranges_equal() || events[0] != (void *)42 ? 1 : 0;
}
