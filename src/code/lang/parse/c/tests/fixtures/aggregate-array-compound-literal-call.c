struct aggregate_array_item {
  unsigned vendor;
  unsigned device;
};

static unsigned selected_device(const struct aggregate_array_item *items,
                                unsigned index) {
  return items[index].device;
}

int main(void) {
  return selected_device(
             (struct aggregate_array_item[]){{.vendor = 1, .device = 2},
                                             {.vendor = 3, .device = 4}},
             1) != 4;
}
