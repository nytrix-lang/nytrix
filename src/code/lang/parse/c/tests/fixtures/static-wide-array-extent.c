/*
 * C parser fixture for inferred static wide-character array extents.
 */
typedef unsigned short nyc_char16_t;

unsigned long nyc_static_wide_array_value(void) {
  static const nyc_char16_t text[] = L"AMI";
  return sizeof(text) + text[0] + text[1] + text[2] + text[3];
}
