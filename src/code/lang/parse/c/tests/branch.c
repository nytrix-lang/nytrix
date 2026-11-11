/*
 * Parser/backend fixture for conditional branches and value selection.
 */
long c_choose(long left, long right) {
  if (left < right) {
    return left + right;
  } else {
    return left - right;
  }
}
