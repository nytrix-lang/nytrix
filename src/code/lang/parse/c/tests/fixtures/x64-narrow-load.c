/*
 * Runtime fixture for narrow scalar loads from x86-64 aggregate fields.
 */
struct nyc_byte_member {
  unsigned char byte;
  unsigned long padding;
};
static struct nyc_byte_member values[2] = {{3, 0}, {41, 0}};
static unsigned int memory_value = 37;
int main(void) {
  unsigned long result;
  asm volatile("movl %[mem], %[out]"
               : [out] "=r"(result)
               : [mem] "m"(memory_value));
  return result != 37 || values[1].byte != 41;
}
