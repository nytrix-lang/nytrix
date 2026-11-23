/* `cond ? floating : integer` converts the integer arm; function-scope
   _Pragma directives other than unroll are ignored, as GCC ignores them. */
double nyr_mixed_double(int c) { return c ? 1.5 : 0; }
float nyr_mixed_float(int c) { return c ? 2.0f : 3; }
double nyr_mixed_widen(int c) { return c ? 2.0 : 3.0f; }
int nyr_pragma_scope(int x) {
  _Pragma("GCC diagnostic push") x++;
  _Pragma("GCC diagnostic pop") return x;
}
