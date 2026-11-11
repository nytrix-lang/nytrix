/*
 * Runtime harness for assembly access to an externally defined global.
 */
unsigned long asm_external_memory_value = 91;
unsigned long read_asm_external_memory(void);
int main(void) { return read_asm_external_memory() != 91; }
