#ifndef NY_DEBUG_H
#define NY_DEBUG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    NY_DBG_NONE        = 0,
    NY_DBG_PASS        = 1u << 0,   // -v   : pass entry/exit, timing
    NY_DBG_IR_DIFF     = 1u << 1,   // -vv  : NYIR before/after each pass
    NY_DBG_CFG         = 1u << 2,   // -vv  : CFG mutations
    NY_DBG_INST        = 1u << 3,   // -vvv : per-instruction rewrites
    NY_DBG_VALUE       = 1u << 4,   // -vvv : value lifetimes, def-use
    NY_DBG_LOWER       = 1u << 5,   // -vvv : NYIR->MachineIR lowering
    NY_DBG_REGALLOC    = 1u << 6,   // -vvvv: register allocation decisions
    NY_DBG_ABI         = 1u << 7,   // -vvvv: ABI classification, stack layout
    NY_DBG_ELF         = 1u << 8,   // -vvvv: ELF section/segment emission
    NY_DBG_TYPE        = 1u << 9,   // -vv  : type inference, unification
    NY_DBG_EFFECT      = 1u << 10,  // -vv  : effect row computation
    NY_DBG_LEX         = 1u << 11,  // -vvv : lexer tokens
    NY_DBG_PARSE       = 1u << 12,  // -vvv : parser AST
    NY_DBG_VERIFY      = 1u << 13,  // -vv  : verification failures
    NY_DBG_ROLLBACK    = 1u << 14,  // -vv  : IR rollback on verify fail
    NY_DBG_ESCAPE      = 1u << 15,  // -vvv : SROA escape analysis
    NY_DBG_IR_FULL     = 1u << 16,  // -vvvv: full IR dump before/after each pass
    NY_DBG_VALUE_TRACE = 1u << 17,  // -vvvv: detailed value lifetime tracing
    NY_DBG_LOWER_DETAILED = 1u << 18, // -vvvv: detailed lowering steps
    NY_DBG_TYPE_DETAIL = 1u << 19,  // -vvvv: detailed type inference steps
    NY_DBG_OPT_MISS    = 1u << 20,  // -vvvv: missed optimization detection
    NY_DBG_MEM         = 1u << 21,  // -vvvv: memory/frame layout
    NY_DBG_HEURISTIC   = 1u << 22,  // -vvvv: per-algorithm decision records
    NY_DBG_TRAIT       = 1u << 23,  // explicit trait/impl resolution trace
    NY_DBG_DEEP        = 1u << 24,  // -vvvvvv: source-local deep diagnostics
} ny_dbg_flag_t;
/* All-bits mask kept out of the enum: an enumerator past INT_MAX is a C23
 * extension and warns on the default toolchain. */
#define NY_DBG_ALL ((ny_dbg_flag_t)0xFFFFFFFFu)

extern ny_dbg_flag_t ny_dbg_enabled;

void ny_dbg_init_from_verbosity(int verbosity);
int ny_dbg_verbosity(void);
/* Scope controls: names the function whose lowering is being traced and
 * marks whether it belongs to the standard library.  Stdlib-scoped debug
 * lines are suppressed below verbosity 6 (-vvvvvv) so user-code problems
 * stay readable at -vvvvv while the full stdlib detail remains one flag
 * away. */
void ny_dbg_lower_fn(const char *name);
void ny_dbg_scope_stdlib(int on);
int ny_dbg_scope_is_stdlib(void);
int ny_dbg_suppressed_by_scope(void);
int ny_dbg_msg_names_module_fn(const char *msg);
void ny_dbg_register_module_prefix(const char *module_name);
void ny_dbg_set_filter(const char *filter);
/* True when an explicit function-only debug filter selects this function,
 * diagnostic message, or current debug context. Raw diagnostic producers use
 * this to keep their sinks scoped with ny_dbg_* records. */
bool ny_dbg_function_selected(const char *function);
void ny_dbg_set_delta_only(bool enabled);
void ny_dbg_set_json(bool enabled);
bool ny_dbg_json_enabled(void);
void ny_dbg_logv(const char *marker, const char *fmt, va_list ap);
void ny_dbg_set_flag(ny_dbg_flag_t flag, bool on);
void ny_dbg_set_color(bool enable);
void ny_dbg_enter_context(const char *context);
void ny_dbg_exit_context(const char *context);
bool ny_dbg_is_enabled(ny_dbg_flag_t flag);

/* Write the crash-forensics log (last compiler decision records) from a
 * fatal-signal handler; async-signal-safe. Call before the crash report. */
void ny_dbg_crash_flush(int sig, const void *fault_addr);
void ny_dbg_print(ny_dbg_flag_t flag, const char *file, int line, const char *fn,
                  const char *fmt, ...);
void ny_dbg_ir_diff(ny_dbg_flag_t flag, const void *func, const char *tag,
                    const void *before, const void *after);
void ny_dbg_inst_rewrite(const void *before, const void *after, const char *reason);
void ny_dbg_value_lifetime(int value, const char *event, const char *detail);
void ny_dbg_lower_step(const char *stage, const void *input, const void *output,
                       const char *detail);
void ny_dbg_lower_instruction(size_t index, const char *op, int dst, int a,
                              int b, int c, int64_t imm,
                              const char *symbol);
bool ny_dbg_lower_sequence_duplicate(size_t length, uint64_t fingerprint);
void ny_dbg_regalloc_decision(int vreg, int preg, const char *reason);
void ny_dbg_regalloc_batch_begin(void);
void ny_dbg_regalloc_batch_end(void);
void ny_dbg_regalloc_block(size_t block, size_t peak_live,
                           size_t live_in, size_t live_out,
                           size_t spills, size_t reloads);
void ny_dbg_abi_classify(const char *name, const char *cls, const char *detail);
void ny_dbg_elf_section(const char *name, size_t offset, size_t size, int flags);
void ny_dbg_elf_reloc(size_t offset, const char *symbol, const char *type);
void ny_dbg_elf_symbol(const char *name, const char *section,
                       size_t value, size_t size);
void ny_dbg_type_unify(const char *a, const char *b, const char *result);
void ny_dbg_effect_row(const char *fn, const char *reads, const char *writes,
                       int alloc);
void ny_dbg_verify_fail(const char *pass, const char *err);
void ny_dbg_rollback(const char *pass);
void ny_dbg_cfg_mutate(const char *mutate, int from_block, int to_block);
void ny_dbg_escape_mark(int slot, const char *reason);
void ny_dbg_sroa_promote(int slot, const char *detail);
void ny_dbg_sccp_overdefined(int value, const char *reason);
void ny_dbg_licm_hoist(int inst_idx, const char *detail);

void ny_dbg_value_trace(int value, const char *event, const char *detail);
void ny_dbg_lower_detailed(const char *stage, const void *input, const void *output,
                           const char *detail);
void ny_dbg_type_detail(const char *phase, const char *detail);
void ny_dbg_trait_detail(const char *detail);
void ny_dbg_opt_missed(const char *pass, const char *pattern, const char *detail);
void ny_dbg_mem_layout(const char *name, size_t offset, size_t size, int align, const char *detail);
void ny_dbg_opt_profile(const char *name, const char *metric, double value);
void ny_dbg_ir_full_dump(const void *func, const char *phase);
void ny_dbg_heuristic_event(const char *algorithm, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define NY_DBG(flag, fmt, ...) \
    do { if (ny_dbg_enabled & (flag)) ny_dbg_print(flag, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)

#define NY_DBG_IR(flag, f, tag, before, after) \
    do { if (ny_dbg_enabled & (flag)) ny_dbg_ir_diff(flag, f, tag, before, after); } while (0)

#define NY_DBG_INST(before, after, reason) \
    do { if (ny_dbg_enabled & NY_DBG_INST) ny_dbg_inst_rewrite(before, after, reason); } while (0)

/* printf-style variant: formats the reason line, then reuses the same
 * rewrite hook. The format buffer is bounded like ny_dbg_print. */
#define NY_DBG_INST_FMT(before, after, fmt, ...) \
    do { \
      if (ny_dbg_enabled & NY_DBG_INST) { \
        char ny_dbg_fmt_buf[256]; \
        snprintf(ny_dbg_fmt_buf, sizeof(ny_dbg_fmt_buf), fmt, ##__VA_ARGS__); \
        ny_dbg_inst_rewrite(before, after, ny_dbg_fmt_buf); \
      } \
    } while (0)

#define NY_DBG_VALUE(v, event, detail) \
    do { if (ny_dbg_enabled & NY_DBG_VALUE) ny_dbg_value_lifetime(v, event, detail); } while (0)

#define NY_DBG_LOWER(stage, in, out, detail) \
    do { if (ny_dbg_enabled & NY_DBG_LOWER) ny_dbg_lower_step(stage, in, out, detail); } while (0)

#define NY_DBG_REGALLOC(vreg, preg, reason) \
    do { if (ny_dbg_enabled & NY_DBG_REGALLOC) ny_dbg_regalloc_decision(vreg, preg, reason); } while (0)

#define NY_DBG_ABI(name, cls, detail) \
    do { if (ny_dbg_enabled & NY_DBG_ABI) ny_dbg_abi_classify(name, cls, detail); } while (0)

#define NY_DBG_ELF(name, offset, size, flags) \
    do { if (ny_dbg_enabled & NY_DBG_ELF) ny_dbg_elf_section(name, offset, size, flags); } while (0)

#define NY_DBG_TYPE(a, b, result) \
    do { if (ny_dbg_enabled & NY_DBG_TYPE) ny_dbg_type_unify(a, b, result); } while (0)

#define NY_DBG_EFFECT(fn, reads, writes, alloc) \
    do { if (ny_dbg_enabled & NY_DBG_EFFECT) ny_dbg_effect_row(fn, reads, writes, alloc); } while (0)

#define NY_DBG_VERIFY(pass, err) \
    do { if (ny_dbg_enabled & NY_DBG_VERIFY) ny_dbg_verify_fail(pass, err); } while (0)

#define NY_DBG_ROLLBACK(pass) \
    do { if (ny_dbg_enabled & NY_DBG_ROLLBACK) ny_dbg_rollback(pass); } while (0)

#define NY_DBG_CFG(mutate, from_block, to_block) \
    do { if (ny_dbg_enabled & NY_DBG_CFG) ny_dbg_cfg_mutate(mutate, from_block, to_block); } while (0)

#define NY_DBG_ESCAPE_MARK(slot, reason) \
    do { if (ny_dbg_enabled & NY_DBG_ESCAPE) ny_dbg_escape_mark(slot, reason); } while (0)

#define NY_DBG_SROA_PROMOTE(slot, detail) \
    do { if (ny_dbg_enabled & NY_DBG_ESCAPE) ny_dbg_sroa_promote(slot, detail); } while (0)

#define NY_DBG_SCCP_OVERDEFINED(value, reason) \
    do { if (ny_dbg_enabled & NY_DBG_VALUE) ny_dbg_sccp_overdefined(value, reason); } while (0)

#define NY_DBG_LICM_HOIST(inst_idx, detail) \
    do { if (ny_dbg_enabled & NY_DBG_INST) ny_dbg_licm_hoist(inst_idx, detail); } while (0)

#define NY_DBG_PROFILE(name, metric, value) \
    do { if (ny_dbg_enabled & NY_DBG_PASS) ny_dbg_profile_metric(name, metric, value); } while (0)

#define NY_DBG_WARN(category, fmt, ...) \
    do { if (ny_dbg_enabled) ny_dbg_warning(category, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)

#define NY_DBG_TRACE_IR(func, phase, detail) \
    do { if (ny_dbg_enabled & (NY_DBG_IR_DIFF|NY_DBG_INST)) ny_dbg_trace_ir(func, phase, detail); } while (0)

#define NY_DBG_OPT_MISS(pass, pattern, detail) \
    do { if (ny_dbg_enabled & (NY_DBG_PASS|NY_DBG_INST)) ny_dbg_opt_missed(pass, pattern, detail); } while (0)

#define NY_DBG_DUMP(tag, data, len) \
    do { if (ny_dbg_enabled) ny_dbg_dump_hex(tag, data, len); } while (0)

#define NY_DBG_VALUE_TRACE(value, event, detail) \
    do { if (ny_dbg_enabled & NY_DBG_VALUE_TRACE) ny_dbg_value_trace(value, event, detail); } while (0)

#define NY_DBG_LOWER_DETAILED(stage, in, out, detail) \
    do { if (ny_dbg_enabled & NY_DBG_LOWER_DETAILED) ny_dbg_lower_detailed(stage, in, out, detail); } while (0)

#define NY_DBG_TYPE_DETAIL(phase, detail) \
    do { if (ny_dbg_enabled & NY_DBG_TYPE_DETAIL) ny_dbg_type_detail(phase, detail); } while (0)

#define NY_DBG_MEM(name, offset, size, align, detail) \
    do { if (ny_dbg_enabled & NY_DBG_MEM) ny_dbg_mem_layout(name, offset, size, align, detail); } while (0)

#define NY_DBG_PROFILE_METRIC(name, metric, value) \
    do { if (ny_dbg_enabled & NY_DBG_PASS) ny_dbg_opt_profile(name, metric, value); } while (0)

#define NY_DBG_IR_FULL(func, phase) \
    do { if (ny_dbg_enabled & NY_DBG_IR_FULL) ny_dbg_ir_full_dump(func, phase); } while (0)

#endif
