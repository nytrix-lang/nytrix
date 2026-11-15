/*
 * Runtime fixture for comma expressions after indexed assignment.
 */
int main(void) {
  int values[2] = {0, 0};
  int index = 0;
  int tail = 0;
  values[index++] = 5, tail++;
  return index != 1 || tail != 1 || values[0] != 5;
}
