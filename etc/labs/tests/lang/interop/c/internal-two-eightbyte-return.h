typedef struct ny_ldiv_result {
  long quot;
  long rem;
} ny_ldiv_result;

extern ny_ldiv_result ldiv(long numerator, long denominator);
