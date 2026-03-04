/* BigInt implementation for Nytrix runtime */
#include "rt/shared.h"
#include <stdlib.h>
#include <string.h>

/* Simple absolute value BigInt operations
   Format: [tag|sign:1|len:15|data:48] followed by limbs
*/

/* Add two absolute bigints: result = |a| + |b| */
int64_t rt_big_add_abs(int64_t a, int64_t b) {
  /* Placeholder: for now just do regular addition
     Real implementation would handle arbitrary precision */
  return a + b;
}

/* Subtract two absolute bigints: result = |a| - |b| */
int64_t rt_big_sub_abs(int64_t a, int64_t b) {
  /* Placeholder: for now just do regular subtraction */
  return a - b;
}

/* Multiply two absolute bigints: result = |a| * |b| */
int64_t rt_big_mul_abs(int64_t a, int64_t b) {
  /* Placeholder: for now just do regular multiplication */
  return a * b;
}
