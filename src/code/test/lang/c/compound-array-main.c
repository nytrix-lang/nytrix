/*
 * Entry point for the compound array C front-end test.
 */
int nyc_compound_array_a(void);
int nyc_compound_array_b(void);

int main(void) {
  return nyc_compound_array_a() != 5 || nyc_compound_array_b() != 11;
}
