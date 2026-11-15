/* Private lowering helpers shared by the C parser's lowering fragments. */
#ifndef NY_CODE_LANG_LOWER_C_LOWER_H
#define NY_CODE_LANG_LOWER_C_LOWER_H

static int normalize_integer(c_parser_t *p, c_type_t type, int value);
static int normalize_u16(c_parser_t *p, int value);
static int normalize_u32(c_parser_t *p, int value);
static uint8_t c_local_memory_flags(const c_parser_t *p, c_type_t type);
static bool lower_store_u8_field(c_parser_t *p, int address, int value,
                                 bool is_volatile);
static int lower_bitfield_binary(c_parser_t *p, nyr_op_t op, int value,
                                 uint64_t constant);
static void find_boot_cpu_capability(c_parser_t *p,
                                     c_global_t *fallback_boot_cpu,
                                     c_agg_field_t *fallback_capability,
                                     c_global_t **boot_cpu,
                                     const c_agg_field_t **capability);
static int emit_u64_compare(c_parser_t *p, int left, int right,
                            nyr_cmp_t comparison);
static int emit_u64_multiply_high(c_parser_t *p, int left, int right);
static c_value_t lower_ctz_value(c_parser_t *p, int input);

#endif
