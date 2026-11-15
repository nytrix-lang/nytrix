/*
 * Fixture: compute the size of an array reached through a pointer field.
 */
struct fifo {
  char (*rectype)[4];
};
unsigned long record_size(struct fifo *fifo) { return sizeof(*fifo->rectype); }
int main(void) { return record_size((struct fifo *)0) != 4; }
