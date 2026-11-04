struct local_pointer_cell { unsigned long value; };
int local_pointer_to_array_read(void) {
  struct local_pointer_cell (*matrix)[3] = 0;
  return matrix == 0;
}
int main(void) { return local_pointer_to_array_read() != 1; }
