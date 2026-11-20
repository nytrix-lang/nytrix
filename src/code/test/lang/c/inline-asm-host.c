/*
 * Host-side oracle for supported inline assembly operations and outputs.
 */
#include <string.h>

int nyc_inline_asm_pause(void);
void nyc_inline_asm_register_to_memory(int *, int);
unsigned int nyc_inline_asm_readl(volatile unsigned int *);
unsigned char nyc_inline_asm_readb(volatile unsigned char *);
unsigned short nyc_inline_asm_readw(volatile unsigned short *);
void nyc_inline_asm_writeb(volatile unsigned char *, unsigned char);
void nyc_inline_asm_writew(volatile unsigned short *, unsigned short);
void nyc_inline_asm_writeq(volatile unsigned long long *, unsigned long long);
int nyc_builtin_isdigit(int);
int nyc_inline_asm_memory_barrier(int *);
int nyc_gnu_statement_expression(int);
int nyc_builtin_expect_side_effect(int *);
int nyc_local_static_pointer_value(void);
int *nyc_local_static_pointer_element_address(long);
int nyc_local_static_external_address(void);
int nyc_external_function_address(void);
const char *nyc_function_name(void);
int nyc_function_section(void);

struct nyc_static_key {
  int value;
};
struct nyc_static_key nyc_kernel_warning_key;
void nyc_external_warning_function(void) {}

int main(void) {
  int value = 37;
  int asm_stored = 0;
  volatile unsigned int asm_loaded = 0x89abcdefu;
  volatile unsigned char asm_byte = 0xa5;
  volatile unsigned short asm_word = 0xbcde;
  volatile unsigned char asm_byte_stored = 0;
  volatile unsigned short asm_word_stored = 0;
  volatile unsigned long long asm_qword_stored = 0;
  int counter = 5;
  nyc_inline_asm_pause();
  nyc_inline_asm_register_to_memory(&asm_stored, 71);
  return asm_stored != 71 || nyc_inline_asm_readl(&asm_loaded) != asm_loaded ||
         nyc_inline_asm_readb(&asm_byte) != asm_byte ||
         nyc_inline_asm_readw(&asm_word) != asm_word ||
         nyc_builtin_isdigit('5') != 1 || nyc_builtin_isdigit('x') != 0 ||
         nyc_builtin_isdigit(-1) != 0 ||
         nyc_inline_asm_memory_barrier(&value) != value ||
         nyc_gnu_statement_expression(20) != 42 ||
         nyc_builtin_expect_side_effect(&counter) != 5 || counter != 6 ||
         nyc_local_static_pointer_value() != 29 ||
         (nyc_inline_asm_writeb(&asm_byte_stored, 0x5a),
          asm_byte_stored != 0x5a) ||
         (nyc_inline_asm_writew(&asm_word_stored, 0x4b3c),
          asm_word_stored != 0x4b3c) ||
         (nyc_inline_asm_writeq(&asm_qword_stored, 0x123456789abcdef0ULL),
          asm_qword_stored != 0x123456789abcdef0ULL) ||
         *nyc_local_static_pointer_element_address(2) != 41 ||
         !nyc_local_static_external_address() ||
         !nyc_external_function_address() ||
         strcmp(nyc_function_name(), "nyc_function_name") != 0 ||
         nyc_function_section() != 43;
}
