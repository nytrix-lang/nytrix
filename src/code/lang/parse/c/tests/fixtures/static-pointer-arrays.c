struct nyc_static_pointer_record { int value; };
static char *nyc_static_character_slots[2];
static struct nyc_static_pointer_record *nyc_static_record_slots[2];
int nyc_static_pointer_arrays_are_zero(void) {
  return nyc_static_character_slots[0] == 0 &&
         nyc_static_character_slots[1] == 0 &&
         nyc_static_record_slots[0] == 0 &&
         nyc_static_record_slots[1] == 0 &&
         &(nyc_static_character_slots)[0] ==
             &nyc_static_character_slots[0];
}
