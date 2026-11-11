/*
 * Runtime harness for multidimensional pointer indexing and array type sizes.
 */
typedef unsigned long nyc_shape_u64;
extern nyc_shape_u64 nyc_shape_values[2][3][4];
extern nyc_shape_u64 nyc_pointer_to_array_index(nyc_shape_u64, nyc_shape_u64,
                                                nyc_shape_u64);
extern nyc_shape_u64 nyc_typeof_array_type_size(void);
nyc_shape_u64 nyc_shape_values[2][3][4];
nyc_shape_u64 nyc_typeof_global_array_index(nyc_shape_u64);
nyc_shape_u64 nyc_typeof_global_values[64];
int main(void) {
  nyc_shape_values[1][2][3] = 0x12345678UL;
  nyc_typeof_global_values[37] = 0x87654321UL;
  return nyc_pointer_to_array_index(1, 2, 3) != 0x12345678UL ||
         nyc_typeof_global_array_index(37) != 0x87654321UL ||
         nyc_typeof_array_type_size() != 512;
}
