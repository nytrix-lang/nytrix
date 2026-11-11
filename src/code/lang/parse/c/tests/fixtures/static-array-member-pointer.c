/*
 * Fixture: initialize and access pointers to members of static aggregates.
 */
struct nyc_entry {
  int value;
};
struct nyc_table {
  struct nyc_entry entries[3];
};
struct nyc_holder {
  struct nyc_entry *entries;
};
static struct nyc_table nyc_source_table;
static struct nyc_holder nyc_holder = {
    .entries = nyc_source_table.entries,
};
int main(void) { return nyc_holder.entries != nyc_source_table.entries; }
