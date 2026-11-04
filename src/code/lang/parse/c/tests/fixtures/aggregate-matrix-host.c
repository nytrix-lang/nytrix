struct cell { unsigned long value; };
struct cell host_matrix[2][3] __asm__("matrix") = {
  {{1}, {2}, {3}}, {{4}, {5}, {42}}
};
unsigned long nyc_read_matrix(void);
int main(void) { return nyc_read_matrix() != 42; }
