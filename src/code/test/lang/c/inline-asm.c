/*
 * Parser/backend fixture for pause and register-to-memory inline assembly.
 */
void nyc_inline_asm_pause(void) { __asm__ __volatile__("pause"); }

void nyc_inline_asm_register_to_memory(int *address, int value) {
  __asm__ __volatile__("movl %0,%1" : : "r"(value), "m"(*address) : "memory");
}

int nyc_builtin_isdigit(int value) { return __builtin_isdigit(value); }

int nyc_inline_asm_outb_compile_only(unsigned char value, unsigned short port) {
  __asm__ volatile("outb %b0, %w1" : : "a"(value), "Nd"(port));
  return 0;
}

int nyc_inline_asm_inl_compile_only(unsigned short port) {
  unsigned int value;
  __asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(port));
  return (int)value;
}

int nyc_inline_asm_inb_compile_only(unsigned short port) {
  unsigned char value;
  __asm__ volatile("inb %w1, %b0" : "=a"(value) : "Nd"(port));
  return value;
}

unsigned int nyc_inline_asm_readl(volatile unsigned int *address) {
  unsigned int value;
  __asm__ volatile("movl %1,%0" : "=r"(value) : "m"(*address) : "memory");
  return value;
}

unsigned char nyc_inline_asm_readb(volatile unsigned char *address) {
  unsigned char value;
  __asm__ volatile("movb %1,%0" : "=q"(value) : "m"(*address) : "memory");
  return value;
}

unsigned short nyc_inline_asm_readw(volatile unsigned short *address) {
  unsigned short value;
  __asm__ volatile("movw %1,%0" : "=r"(value) : "m"(*address) : "memory");
  return value;
}

void nyc_inline_asm_writeb(volatile unsigned char *address,
                           unsigned char value) {
  __asm__ volatile("movb %0,%1" : : "q"(value), "m"(*address) : "memory");
}

void nyc_inline_asm_writew(volatile unsigned short *address,
                           unsigned short value) {
  __asm__ volatile("movw %0,%1" : : "r"(value), "m"(*address) : "memory");
}

void nyc_inline_asm_writeq(volatile unsigned long long *address,
                           unsigned long long value) {
  __asm__ volatile("movq %0,%1" : : "r"(value), "m"(*address) : "memory");
}

int nyc_inline_asm_memory_barrier(int *value) {
  int result = *value;
  __asm__ __volatile__("" ::
                           : "mem"
                             "ory");
  return result;
}

int nyc_gnu_statement_expression(int value) {
  return ({
    int adjusted = value + 1;
    adjusted * 2;
  });
}

int nyc_builtin_expect_side_effect(int *value) {
  return __builtin_expect((*value)++, 0);
}

static int nyc_static_pointer_target = 29;
typedef unsigned long uintptr_t;
struct nyc_static_key;
extern struct nyc_static_key nyc_kernel_warning_key;
extern void nyc_external_warning_function(void);

int nyc_local_static_pointer_value(void) {
  static int *target = &nyc_static_pointer_target;
  return *target;
}

int *nyc_local_static_pointer_element_address(long index) {
  static int values[] = {17, 29, 41};
  static int *target = values;
  return &target[index];
}

int nyc_local_static_external_address(void) {
  static void *address = (void *)(uintptr_t)&nyc_kernel_warning_key;
  return address == &nyc_kernel_warning_key;
}

int nyc_external_function_address(void) {
  return &nyc_external_warning_function != 0;
}

const char *nyc_function_name(void) { return __func__; }

int __attribute__((__section__(".text.nyc_inline_asm_function_section")))
nyc_function_section(void) {
  return 43;
}
