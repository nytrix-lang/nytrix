/*
 * Fixture: access a file-local global from position-independent code.
 */
static unsigned long pic_value = 37;
unsigned long read_pic_value(void) { return pic_value; }
int main(void) { return read_pic_value() != 37; }
