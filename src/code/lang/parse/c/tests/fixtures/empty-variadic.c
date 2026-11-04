void empty_variadic(int fixed, ...) { (void)fixed; }
int main(void) {
  empty_variadic(7, 11, 13);
  return 0;
}
