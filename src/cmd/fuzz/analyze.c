#include "core.h"

static bool line_has_call_symbol(const char *line, size_t len, const char *symbol) {
  if (!memmem(line, len, "call", 4)) return false;
  size_t sym_len = strlen(symbol);
  for (size_t i = 0; i + sym_len + 1 <= len; ++i) {
    if (line[i] != '@') continue;
    if (memcmp(line + i + 1, symbol, sym_len) != 0) continue;
    char next = (i + 1 + sym_len < len) ? line[i + 1 + sym_len] : '\0';
    if (!ident_char(next)) return true;
  }
  return false;
}

static int call_count(const char *data, size_t len, const char *symbol) {
  int count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i == len || data[i] == '\n') {
      if (line_has_call_symbol(data + start, i - start, symbol)) ++count;
      start = i + 1;
    }
  }
  return count;
}

static bool call_prefix_on_line(const char *line, size_t len, const char *prefix) {
  if (!memmem(line, len, "call", 4)) return false;
  size_t prefix_len = strlen(prefix);
  for (size_t i = 0; i + prefix_len + 1 <= len; ++i) {
    if (line[i] == '@' && memcmp(line + i + 1, prefix, prefix_len) == 0) return true;
  }
  return false;
}

static int call_prefix_count(const char *data, size_t len, const char *prefix) {
  int count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i == len || data[i] == '\n') {
      if (call_prefix_on_line(data + start, i - start, prefix)) ++count;
      start = i + 1;
    }
  }
  return count;
}

int cmd_hash_case(const char *path) {
  file_buf_t f = {0};
  if (!read_file(path, &f)) {
    printf("{\"ok\":false,\"error\":\"read-failed\",\"path\":");
    json_str(stdout, path);
    printf("}\n");
    return 1;
  }
  uint64_t h = fnv1a64(f.data, f.len);
  printf("{\"ok\":true,\"path\":");
  json_str(stdout, path);
  printf(",\"bytes\":%zu,\"fnv1a64\":\"%016" PRIx64 "\"}\n", f.len, h);
  free(f.data);
  return 0;
}

int cmd_scan_ir_markers(const char *path) {
  file_buf_t f = {0};
  if (!read_file(path, &f)) {
    printf("{\"ok\":false,\"error\":\"read-failed\",\"path\":");
    json_str(stdout, path);
    printf("}\n");
    return 1;
  }
  printf("{\"ok\":true,\"path\":");
  json_str(stdout, path);
  printf(",\"bytes\":%zu,\"markers\":{"
         "\"rt_list_new\":%d,\"rt_store_item_fast\":%d,\"rt_list_set_len\":%d,"
         "\"rt_is_float_obj\":%d,\"rt_flt_to_int\":%d,\"rt_mod\":%d,"
         "\"std_core_reflect\":%d,\"proven_int_cast_fast\":%d,"
         "\"proven_int_branch_fast\":%d,\"proven_int_mod_fast\":%d,"
         "\"raw_int_list_get\":%d,\"raw_int_list_set\":%d,"
         "\"static_int_list_untagged\":%d,\"raw_int_list_untagged\":%d,"
         "\"llvm_loop\":%d,\"vector_body\":%d}}\n",
         f.len,
         count_sub(f.data, f.len, "rt_list_new"),
         count_sub(f.data, f.len, "rt_store_item_fast"),
         count_sub(f.data, f.len, "rt_list_set_len"),
         count_sub(f.data, f.len, "rt_is_float_obj"),
         count_sub(f.data, f.len, "rt_flt_to_int"),
         count_sub(f.data, f.len, "rt_mod"),
         count_sub(f.data, f.len, "std.core.reflect"),
         count_sub(f.data, f.len, "proven_int_cast_fast"),
         count_sub(f.data, f.len, "proven_int_branch_fast"),
         count_sub(f.data, f.len, "proven_int_mod_fast"),
         count_sub(f.data, f.len, "raw_int_list_get"),
         count_sub(f.data, f.len, "raw_int_list_set"),
         count_sub(f.data, f.len, "static_int_list_untagged"),
         count_sub(f.data, f.len, "raw_int_list_untagged"),
         count_sub(f.data, f.len, "!llvm.loop"),
         count_sub(f.data, f.len, "vector.body"));
  free(f.data);
  return 0;
}

static void print_blockers(FILE *out, int hot_runtime_calls, int user_reflective_calls, int rt_mod_calls,
                           int std_core_mod_calls, int float_guards, int reflect_arith_calls,
                           int vector_markers, int list_new_calls, int list_store_helper_calls,
                           int list_mutation_helper_calls, int tagged_int_op_count,
                           int top_entry_string_init_count) {
  bool first = true;
#define ADD_BLOCKER_FMT(cond, fmt, value) \
  do { if (cond) { if (!first) fputc(',', out); first = false; fputc('"', out); fprintf(out, fmt, value); fputc('"', out); } } while (0)
#define ADD_BLOCKER_STR(cond, value) \
  do { if (cond) { if (!first) fputc(',', out); first = false; json_str(out, value); } } while (0)
  ADD_BLOCKER_FMT(hot_runtime_calls, "hot-runtime-calls:%d", hot_runtime_calls);
  ADD_BLOCKER_FMT(user_reflective_calls, "reflective-calls:%d", user_reflective_calls);
  ADD_BLOCKER_FMT(rt_mod_calls, "rt-mod:%d", rt_mod_calls);
  ADD_BLOCKER_FMT(std_core_mod_calls, "std-core-mod:%d", std_core_mod_calls);
  ADD_BLOCKER_FMT(float_guards, "float-int-guards:%d", float_guards);
  ADD_BLOCKER_FMT(reflect_arith_calls, "reflect-arith:%d", reflect_arith_calls);
  ADD_BLOCKER_STR(!vector_markers, "no-vector-markers");
  ADD_BLOCKER_STR(list_new_calls, "list-construction");
  ADD_BLOCKER_STR(list_store_helper_calls, "list-store-helper");
  ADD_BLOCKER_STR(list_mutation_helper_calls, "list-mutation");
  ADD_BLOCKER_FMT(tagged_int_op_count >= 4 && !hot_runtime_calls && !user_reflective_calls, "tagged-int-arithmetic:%d", tagged_int_op_count);
  ADD_BLOCKER_FMT(top_entry_string_init_count >= 64, "top-entry-string-init:%d", top_entry_string_init_count);
#undef ADD_BLOCKER_FMT
#undef ADD_BLOCKER_STR
}

int cmd_analyze_ir(const char *path) {
  file_buf_t f = {0};
  if (!read_file(path, &f)) {
    printf("{\"ok\":false,\"error\":\"read-failed\",\"path\":");
    json_str(stdout, path);
    printf("}\n");
    return 1;
  }
  int runtime_calls = call_prefix_count(f.data, f.len, "rt_");
  int reflective_calls = call_prefix_count(f.data, f.len, "std.core.reflect.");
  int output_calls = call_count(f.data, f.len, "rt_print_int") +
                     call_count(f.data, f.len, "rt_print_newline") +
                     call_count(f.data, f.len, "rt_print_str_raw") +
                     call_count(f.data, f.len, "rt_to_str");
  int hot_runtime_calls = runtime_calls > output_calls ? runtime_calls - output_calls : 0;
  int rt_mod_calls = call_count(f.data, f.len, "rt_mod");
  int std_core_mod_calls = call_count(f.data, f.len, "std.core.mod");
  int rt_is_float_obj_calls = call_count(f.data, f.len, "rt_is_float_obj");
  int rt_flt_to_int_calls = call_count(f.data, f.len, "rt_flt_to_int");
  int reflect_add_calls = call_count(f.data, f.len, "std.core.reflect.add");
  int reflect_sub_calls = call_count(f.data, f.len, "std.core.reflect.sub");
  int reflect_mul_calls = call_count(f.data, f.len, "std.core.reflect.mul");
  int reflect_div_calls = call_count(f.data, f.len, "std.core.reflect.div");
  int reflect_mod_calls = call_count(f.data, f.len, "std.core.reflect.mod");
  int reflect_arith_calls = reflect_add_calls + reflect_sub_calls + reflect_mul_calls + reflect_div_calls + reflect_mod_calls;
  int list_new_calls = call_count(f.data, f.len, "rt_list_new");
  int list_store_helper_calls = call_count(f.data, f.len, "rt_store_item_fast");
  int list_set_len_calls = call_count(f.data, f.len, "rt_list_set_len");
  int list_mutation_helper_calls = list_store_helper_calls + call_count(f.data, f.len, "rt_store_item");
  int static_int_lists = count_sub(f.data, f.len, "@__ny_static_int_list_");
  int static_get_loads = count_sub(f.data, f.len, "static_get_elem");
  int trusted_get_loads = count_sub(f.data, f.len, "trusted_get_inbounds_elem");
  int raw_gets = count_sub(f.data, f.len, "raw_int_list_get");
  int raw_sets = count_sub(f.data, f.len, "raw_int_list_set");
  int raw_gets_inbounds = count_sub(f.data, f.len, "raw_int_list_get_inbounds");
  int raw_sets_inbounds = count_sub(f.data, f.len, "raw_int_list_set_inbounds");
  int raw_guard_blocks = count_sub(f.data, f.len, "raw_list_get.") + count_sub(f.data, f.len, "raw_list_set.");
  int raw_panic_edges = count_sub(f.data, f.len, "raw_list_set.panic");
  int proven_branch = count_sub(f.data, f.len, "proven_int_branch_fast");
  int proven_cast = count_sub(f.data, f.len, "proven_int_cast_fast") + count_sub(f.data, f.len, "__ny_diag_proven_int_cast_fast_");
  int proven_mod = count_sub(f.data, f.len, "proven_int_mod");
  int raw_expr_add = count_sub(f.data, f.len, "raw_int_expr_fast_add");
  int raw_expr_sub = count_sub(f.data, f.len, "raw_int_expr_fast_sub");
  int raw_expr_mul = count_sub(f.data, f.len, "raw_int_expr_fast_mul");
  int raw_expr_mod = count_sub(f.data, f.len, "raw_int_expr_fast_mod");
  int raw_expr_total = raw_expr_add + raw_expr_sub + raw_expr_mul + raw_expr_mod;
  int untagged_lists = count_sub(f.data, f.len, "static_int_list_untagged") + count_sub(f.data, f.len, "raw_int_list_untagged");
  int vector_markers = count_sub(f.data, f.len, "vector.body") +
                       count_sub(f.data, f.len, "llvm.loop.vectorize.enable") +
                       count_sub(f.data, f.len, "llvm.vector.reduce");
  int loop_markers = count_sub(f.data, f.len, "!llvm.loop");
  int tagged_untag = count_sub(f.data, f.len, " ashr ") + count_sub(f.data, f.len, " lshr ");
  int tagged_retag = count_sub(f.data, f.len, " shl ");
  int tagged_or = count_sub(f.data, f.len, " or ");
  int tagged_and = count_sub(f.data, f.len, " and ");
  int tagged_ops = tagged_untag + tagged_retag + tagged_or + tagged_and;
  int top_entry_string_inits = count_sub(f.data, f.len, "@.str.runtime.");
  int top_entry_stores = count_sub(f.data, f.len, "\n  store ");
  int const_string_inits = count_sub(f.data, f.len, "ptrtoint (");
  printf("{\"ok\":true,\"analysis_engine\":\"nynth_core\",\"runtime_call_count\":%d,"
         "\"reflective_call_count\":%d,\"user_runtime_call_count\":%d,"
         "\"user_reflective_call_count\":%d,\"output_runtime_call_count\":%d,"
         "\"hot_runtime_call_count\":%d,\"print_nil_check_count\":%d,"
         "\"print_dynamic_fallback_block_count\":%d,\"print_to_str_call_count\":%d,"
         "\"print_direct_int_call_count\":%d,\"rt_mod_count\":%d,\"std_core_mod_count\":%d,"
         "\"proven_int_mod_count\":%d,\"rt_is_float_obj_count\":%d,\"rt_flt_to_int_count\":%d,"
         "\"reflect_add_count\":%d,\"reflect_sub_count\":%d,\"reflect_mul_count\":%d,"
         "\"reflect_div_count\":%d,\"reflect_mod_count\":%d,\"reflect_arith_count\":%d,"
         "\"static_int_list_count\":%d,\"static_get_load_count\":%d,"
         "\"static_list_elide_lowered\":%d,\"static_list_elide_optimized_away\":0,"
         "\"static_list_elide_bail_reason\":%s,\"trusted_get_load_count\":%d,"
         "\"raw_mutation_get_count\":%d,\"raw_mutation_set_count\":%d,"
         "\"raw_mutation_guard_blocks\":%d,\"raw_mutation_panic_edges\":%d,"
         "\"raw_mutation_inbounds_gets\":%d,\"raw_mutation_inbounds_sets\":%d,"
         "\"raw_mutation_lowered\":%d,\"raw_mutation_bail_reason\":%s,"
         "\"proven_int_branch_fast_count\":%d,\"proven_int_cast_fast_count\":%d,"
         "\"raw_int_expr_fast_count\":%d,\"raw_int_expr_fast_add_count\":%d,"
         "\"raw_int_expr_fast_sub_count\":%d,\"raw_int_expr_fast_mul_count\":%d,"
         "\"raw_int_expr_fast_mod_count\":%d,\"tagged_int_untag_shift_count\":%d,"
         "\"tagged_int_retag_shift_count\":%d,\"tagged_int_tag_or_count\":%d,"
         "\"tagged_int_lowbit_check_count\":%d,\"tagged_int_op_count\":%d,"
         "\"top_entry_store_count\":%d,\"top_entry_string_init_count\":%d,"
         "\"const_string_global_init_count\":%d,\"string_runtime_global_count\":%d,"
         "\"untagged_list_count\":%d,\"list_new_call_count\":%d,"
         "\"list_store_helper_count\":%d,\"list_set_len_call_count\":%d,"
         "\"list_mutation_helper_count\":%d,\"vector_marker_count\":%d,"
         "\"loop_metadata_count\":%d,\"vectorized\":%s,\"blockers\":[",
         runtime_calls, reflective_calls, runtime_calls, reflective_calls, output_calls,
         hot_runtime_calls, count_sub(f.data, f.len, "print_is_nil"),
         count_sub(f.data, f.len, "\nprint_other:"), call_count(f.data, f.len, "rt_to_str"),
         call_count(f.data, f.len, "rt_print_int"), rt_mod_calls, std_core_mod_calls,
         proven_mod, rt_is_float_obj_calls, rt_flt_to_int_calls,
         reflect_add_calls, reflect_sub_calls, reflect_mul_calls, reflect_div_calls,
         reflect_mod_calls, reflect_arith_calls, static_int_lists, static_get_loads,
         (static_int_lists > 0 && static_get_loads > 0 && !list_store_helper_calls) ? 1 : 0,
         (static_int_lists > 0 && static_get_loads > 0 && !list_store_helper_calls) ? "\"\"" : "\"not-lowered-or-not-candidate\"",
         trusted_get_loads, raw_gets, raw_sets, raw_guard_blocks, raw_panic_edges,
         raw_gets_inbounds, raw_sets_inbounds, raw_gets + raw_sets,
         (raw_gets + raw_sets) ? "\"\"" : "\"not-lowered-or-not-candidate\"",
         proven_branch, proven_cast, raw_expr_total, raw_expr_add, raw_expr_sub, raw_expr_mul,
         raw_expr_mod, tagged_untag, tagged_retag, tagged_or, tagged_and, tagged_ops,
         top_entry_stores, top_entry_string_inits, const_string_inits,
         count_sub(f.data, f.len, "@.str.runtime."), untagged_lists,
         list_new_calls, list_store_helper_calls, list_set_len_calls,
         list_mutation_helper_calls, vector_markers, loop_markers,
         vector_markers ? "true" : "false");
  print_blockers(stdout, hot_runtime_calls, reflective_calls, rt_mod_calls, std_core_mod_calls,
                 rt_is_float_obj_calls + rt_flt_to_int_calls, reflect_arith_calls,
                 vector_markers, list_new_calls, list_store_helper_calls,
                 list_mutation_helper_calls, tagged_ops, top_entry_string_inits);
  printf("]}\n");
  free(f.data);
  return 0;
}
