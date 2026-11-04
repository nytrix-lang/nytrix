static const unsigned int nyc_designated_values[] = {
  [2] = 37,
  41,
  [5] = 53,
};

int main(void) {
  return nyc_designated_values[0] != 0 ||
         nyc_designated_values[1] != 0 ||
         nyc_designated_values[2] != 37 ||
         nyc_designated_values[3] != 41 ||
         nyc_designated_values[4] != 0 ||
         nyc_designated_values[5] != 53;
}
