/*
 * Fixture: write through aliases of a global character array.
 */
static char data[200];
static char *alias = data;
int main(void) {
  const char *src = "abc";
  int i = 0;
  while (*src)
    data[i++] = *src++;
  data[i] = 0;
  return alias[0] != 'a' || alias[1] != 'b' || alias[2] != 'c' || alias[3] != 0;
}
