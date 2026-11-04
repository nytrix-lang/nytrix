float c_float_roundtrip(float value) {
  float result = value + 0.5f;
  return result;
}

float c_float_negate(float value) { return -value; }

float c_float_select(long condition, float left, float right) {
  return condition ? left : right;
}

float c_float_pointer(float *values, float value) {
  values[0] = value;
  values[0] += 0.5f;
  return *values;
}
