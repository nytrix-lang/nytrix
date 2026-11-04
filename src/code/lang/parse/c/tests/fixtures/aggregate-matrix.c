struct cell { unsigned long value; };
extern struct cell matrix[2][3];
unsigned long nyc_read_matrix(void) {
  return matrix[1][2].value;
}
