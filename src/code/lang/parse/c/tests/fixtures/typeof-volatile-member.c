struct nyc_typeof_store { unsigned char holdout; };
static struct nyc_typeof_store nyc_typeof_store_state;
static inline struct nyc_typeof_store *nyc_typeof_store_current(void) {
  return &nyc_typeof_store_state;
}
void nyc_typeof_volatile_member_store(unsigned char value) {
  *(volatile typeof(nyc_typeof_store_current()->holdout) *)
      &nyc_typeof_store_current()->holdout = value;
}
unsigned char nyc_typeof_volatile_member_value(void) {
  return nyc_typeof_store_current()->holdout;
}
