long c_locals(long left, long right) {
  long sum = left + right;
  long doubled = sum * 2;
  return doubled - left;
}

long c_many_locals() {
  long v0 = 0;
  long v1 = 1;
  long v2 = 2;
  long v3 = 3;
  long v4 = 4;
  long v5 = 5;
  long v6 = 6;
  long v7 = 7;
  long v8 = 8;
  long v9 = 9;
  long v10 = 10;
  long v11 = 11;
  long v12 = 12;
  long v13 = 13;
  long v14 = 14;
  long v15 = 15;
  long v16 = 16;
  long v17 = 17;
  long v18 = 18;
  long v19 = 19;
  long v20 = 20;
  long v21 = 21;
  long v22 = 22;
  long v23 = 23;
  long v24 = 24;
  long v25 = 25;
  long v26 = 26;
  long v27 = 27;
  long v28 = 28;
  long v29 = 29;
  long v30 = 30;
  long v31 = 31;
  long v32 = 32;
  return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 +
         v13 + v14 + v15 + v16 + v17 + v18 + v19 + v20 + v21 + v22 + v23 + v24 +
         v25 + v26 + v27 + v28 + v29 + v30 + v31 + v32;
}

double c_double_local(double left, double right) {
  double total = left + right;
  return total * 2.0;
}

long c_local_update(long left, long right) {
  long value = left;
  value = value + right;
  return value;
}

long c_multi_declarator(long seed) {
  long first = seed, second = seed + 2, result;
  result = first + second;
  return result;
}

long c_const_multi_missing_initializer() {
  const long first = 1, second;
  return first + second;
}

long c_declare_then_assign(long left, long right) {
  long value;
  value = left + right;
  return value;
}

long c_parameter_update(long value) {
  value = value + 1;
  return value;
}

double c_double_local_update(double left, double right) {
  double value = left;
  value = value + right;
  return value * 2.0;
}
