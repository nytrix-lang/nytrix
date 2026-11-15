/*
 * C fixture for pointer arithmetic across multidimensional arrays.
 */
typedef unsigned long nyc_shape_u64;
extern nyc_shape_u64 nyc_shape_values[2][3][4];
extern __typeof__(nyc_shape_u64[64]) nyc_typeof_global_values;
nyc_shape_u64 nyc_pointer_to_array_index(nyc_shape_u64 i, nyc_shape_u64 j,
                                         nyc_shape_u64 k) {
  __typeof__(&nyc_shape_values) pointer = &nyc_shape_values;
  return (*pointer)[i][j][k];
}
nyc_shape_u64 nyc_typeof_global_array_index(nyc_shape_u64 i) {
  (void)&(nyc_typeof_global_values[i]);
  return nyc_typeof_global_values[i];
}
nyc_shape_u64 nyc_typeof_array_type_size(void) {
  return sizeof(__typeof__(nyc_shape_u64[64]));
}
