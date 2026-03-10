;; Keywords: smt satisfiability solver
;; SMT utilities (Z3)
module std.math.smt(z3_available, z3_version, z3_version_str, z3_global_param_set,
   z3_global_timeout_ms, z3_ctx_new, z3_ctx_del, z3_ctx_last_error,
   z3_solver_new, z3_solver_new_for_logic, z3_solver_del, z3_solver_from_string,
   z3_bv_sort, z3_int_sort, z3_sym, z3_bv_const, z3_int_const, z3_int_val,
   z3_bv_u64, z3_bvextract, z3_bvconcat, z3_eq, z3_int_add, z3_int_sub, z3_int_mul,
   z3_int_mod, z3_int_le, z3_int_ge, z3_bvadd, z3_bvsub, z3_bvmul,
   z3_bvxor, z3_bvand, z3_bvor, z3_bvshl, z3_bvlshr, z3_bvashr,
   z3_bvnot, z3_bvneg, z3_zero_extend, z3_sign_extend, z3_mk_or, z3_mk_and,
   z3_bvudiv, z3_bvsdiv, z3_bvurem, z3_bvsrem, z3_bvule, z3_bvult,
   z3_bvuge, z3_bvugt, z3_bvsle, z3_bvslt, z3_bvsge, z3_bvsgt,
   z3_solver_assert, z3_solver_check, z3_model_eval_u64,
   z3_tactic_new, z3_tactic_del, z3_tactic_and_then, z3_tactic_try_for,
   z3_tactic_using_params, z3_solver_new_from_tactic,
   SAT, UNSAT, UNKNOWN, global_param_set, global_timeout_ms,
   ctx_new, ctx_del, ctx_last_error, solver_new, solver_new_for_logic, solver_new_qfbv,
   tactic_new, tactic_del, tactic_and_then, tactic_chain, tactic_try_for, tactic_using_params,
   solver_new_from_tactic, solver_new_tactic_named, solver_new_tactic_chain, solver_new_qfbv_tactic,
   solver_new_qfbv_sls,
   solver_del, solver_from_string, bv_sort, int_sort, sym, bv_const, int_const, int_val, bv_u64,
   bv_extract, bvconcat, bv_hex, mk_eq, mk_not, int_add, int_sub, int_mul, int_mod, int_le, int_ge, bvadd,
   bvsub, bvmul, bvxor, bvand, bvor, bvshl, bvlshr, bvashr, bvnot, bvneg, bvudiv, bvsdiv, bvurem, bvsrem,
   bvzext, bvsext, bvule, bvult, bvuge, bvugt, bvsle, bvslt, bvsge, bvsgt, mk_neq, mk_or, mk_and,
   solver_assert, solver_check, solver_check_result, solver_set_timeout_ms, model_eval_u64,
   model_eval_hex, hex_width, model_eval_hex_width, bv_u8, bv_u16, bv_u32, bv_bytes, bv_words, solver_assert_bytes_eq,
   solver_assert_bytes_xor_eq, solver_assert_bytes_xor_reduce8, solver_assert_bytes_add_sum8,
   solver_assert_bytes_ascii_range, bvrotl, bvrotr, model_eval_bytes, model_eval_bytes_hex,
   model_eval_ascii, model_eval_ascii_checked, const_decl, model_eval_decl_bv_hex, solver_assert_decl_not_hex,
   parser_ctx_new, parser_ctx_free,
   parser_ctx_add_decl, parser_ctx_parse, solver_assert_ast_vector, ast_vector_free, session_new,
   session_free, session_ctx, session_solver, session_check, solve_qf_bv_const_hex, solve_qf_bv_bytes,
solve_qf_bv_bytes_hex, solve_qf_bv_ascii)

use std.core
use std.os.ffi (
   dlsym, call0, call0_ptr, call1, call1_ptr, call2, call2_ptr, call2_ptr_u32, call3, call3_ptr,
   call3_ptr_u64_ptr, call3_ptr_u32_ptr, call3_ptr_ptr_u32, call4, call4_ptr, call5, call5_ptr,
   call4_ptr_ptr_ptr_ptr_void, malloc, free, cstr,
)

use std.core.str as str
use std.math.backends as backends

def SAT     = 1
def UNSAT   = -1
def UNKNOWN = 0

if(comptime{ __os_name() == "linux" }){
   #link "libz3.so"
   #include <z3.h> as "Z3_"
}

if(comptime{ __os_name() == "macos" }){
   #link "libz3.dylib"
   #include <z3.h> as "Z3_"
}

if(comptime{ __os_name() == "windows" }){
   #link "z3.lib"
   #include <z3.h> as "Z3_"
}

mut _z3 = 0
mut _p_global_param_set = 0
mut _p_get_version = 0
mut _p_mk_config = 0
mut _p_del_config = 0
mut _p_mk_context = 0
mut _p_del_context = 0
mut _p_get_error_code = 0
mut _p_get_error_msg = 0
mut _p_mk_solver = 0
mut _p_mk_solver_for_logic = 0
mut _p_solver_inc_ref = 0
mut _p_solver_dec_ref = 0
mut _p_mk_bv_sort = 0
mut _p_mk_int_sort = 0
mut _p_mk_string_symbol = 0
mut _p_mk_const = 0
mut _p_mk_unsigned_int64 = 0
mut _p_mk_eq = 0
mut _p_mk_add = 0
mut _p_mk_sub = 0
mut _p_mk_mul = 0
mut _p_mk_mod = 0
mut _p_mk_le = 0
mut _p_mk_ge = 0
mut _p_mk_extract = 0
mut _p_mk_concat = 0
mut _p_mk_bvadd = 0
mut _p_mk_bvsub = 0
mut _p_mk_bvmul = 0
mut _p_mk_bvxor = 0
mut _p_mk_bvand = 0
mut _p_mk_bvor = 0
mut _p_mk_bvshl = 0
mut _p_mk_bvlshr = 0
mut _p_mk_bvashr = 0
mut _p_mk_bvnot = 0
mut _p_mk_bvneg = 0
mut _p_mk_zero_ext = 0
mut _p_mk_sign_ext = 0
mut _p_mk_or = 0
mut _p_mk_and = 0
mut _p_mk_bvudiv = 0
mut _p_mk_bvsdiv = 0
mut _p_mk_bvurem = 0
mut _p_mk_bvsrem = 0
mut _p_mk_bvule = 0
mut _p_mk_bvult = 0
mut _p_mk_bvuge = 0
mut _p_mk_bvugt = 0
mut _p_mk_bvsle = 0
mut _p_mk_bvslt = 0
mut _p_mk_bvsge = 0
mut _p_mk_bvsgt = 0
mut _p_solver_assert = 0
mut _p_solver_check = 0
mut _p_solver_from_string = 0
mut _p_solver_get_model = 0
mut _p_model_inc_ref = 0
mut _p_model_dec_ref = 0
mut _p_model_eval = 0
mut _p_get_numeral_uint64 = 0
mut _p_mk_numeral = 0
mut _p_mk_not = 0
mut _p_mk_params = 0
mut _p_params_inc_ref = 0
mut _p_params_dec_ref = 0
mut _p_params_set_uint = 0
mut _p_params_set_bool = 0
mut _p_solver_set_params = 0
mut _p_mk_tactic = 0
mut _p_tactic_inc_ref = 0
mut _p_tactic_dec_ref = 0
mut _p_tactic_and_then = 0
mut _p_tactic_try_for = 0
mut _p_tactic_using_params = 0
mut _p_mk_solver_from_tactic = 0
mut _p_get_app_decl = 0
mut _p_ast_to_string = 0
mut _p_model_get_const_interp = 0
mut _p_parser_ctx_new = 0
mut _p_parser_ctx_inc_ref = 0
mut _p_parser_ctx_del = 0
mut _p_parser_ctx_add_decl = 0
mut _p_parser_ctx_from_string = 0
mut _p_ast_vector_size = 0
mut _p_ast_vector_get = 0
mut _p_ast_vector_inc_ref = 0
mut _p_ast_vector_dec_ref = 0

fn _is_handle(any: h): bool { h != 0 }

fn _ffi_int(any: v): int {
   (int(v) << 1) | 1
}

fn _load(): bool {
   if(_z3){ return true }
   def h = backends.backend_dlopen_checked("z3", "z3", "Z3_mk_config")
   if(!h){ return false }
   _z3 = h
   _p_global_param_set = dlsym(h, "Z3_global_param_set")
   _p_get_version = dlsym(h, "Z3_get_version")
   _p_mk_config = dlsym(h, "Z3_mk_config")
   _p_del_config = dlsym(h, "Z3_del_config")
   _p_mk_context = dlsym(h, "Z3_mk_context")
   _p_del_context = dlsym(h, "Z3_del_context")
   _p_get_error_code = dlsym(h, "Z3_get_error_code")
   _p_get_error_msg = dlsym(h, "Z3_get_error_msg")
   _p_mk_solver = dlsym(h, "Z3_mk_solver")
   _p_mk_solver_for_logic = dlsym(h, "Z3_mk_solver_for_logic")
   _p_solver_inc_ref = dlsym(h, "Z3_solver_inc_ref")
   _p_solver_dec_ref = dlsym(h, "Z3_solver_dec_ref")
   _p_mk_bv_sort = dlsym(h, "Z3_mk_bv_sort")
   _p_mk_int_sort = dlsym(h, "Z3_mk_int_sort")
   _p_mk_string_symbol = dlsym(h, "Z3_mk_string_symbol")
   _p_mk_const = dlsym(h, "Z3_mk_const")
   _p_mk_unsigned_int64 = dlsym(h, "Z3_mk_unsigned_int64")
   _p_mk_eq = dlsym(h, "Z3_mk_eq")
   _p_mk_add = dlsym(h, "Z3_mk_add")
   _p_mk_sub = dlsym(h, "Z3_mk_sub")
   _p_mk_mul = dlsym(h, "Z3_mk_mul")
   _p_mk_mod = dlsym(h, "Z3_mk_mod")
   _p_mk_le = dlsym(h, "Z3_mk_le")
   _p_mk_ge = dlsym(h, "Z3_mk_ge")
   _p_mk_extract = dlsym(h, "Z3_mk_extract")
   _p_mk_concat = dlsym(h, "Z3_mk_concat")
   _p_mk_bvadd = dlsym(h, "Z3_mk_bvadd")
   _p_mk_bvsub = dlsym(h, "Z3_mk_bvsub")
   _p_mk_bvmul = dlsym(h, "Z3_mk_bvmul")
   _p_mk_bvxor = dlsym(h, "Z3_mk_bvxor")
   _p_mk_bvand = dlsym(h, "Z3_mk_bvand")
   _p_mk_bvor = dlsym(h, "Z3_mk_bvor")
   _p_mk_bvshl = dlsym(h, "Z3_mk_bvshl")
   _p_mk_bvlshr = dlsym(h, "Z3_mk_bvlshr")
   _p_mk_bvashr = dlsym(h, "Z3_mk_bvashr")
   _p_mk_bvnot = dlsym(h, "Z3_mk_bvnot")
   _p_mk_bvneg = dlsym(h, "Z3_mk_bvneg")
   _p_mk_zero_ext = dlsym(h, "Z3_mk_zero_ext")
   _p_mk_sign_ext = dlsym(h, "Z3_mk_sign_ext")
   _p_mk_or = dlsym(h, "Z3_mk_or")
   _p_mk_and = dlsym(h, "Z3_mk_and")
   _p_mk_bvudiv = dlsym(h, "Z3_mk_bvudiv")
   _p_mk_bvsdiv = dlsym(h, "Z3_mk_bvsdiv")
   _p_mk_bvurem = dlsym(h, "Z3_mk_bvurem")
   _p_mk_bvsrem = dlsym(h, "Z3_mk_bvsrem")
   _p_mk_bvule = dlsym(h, "Z3_mk_bvule")
   _p_mk_bvult = dlsym(h, "Z3_mk_bvult")
   _p_mk_bvuge = dlsym(h, "Z3_mk_bvuge")
   _p_mk_bvugt = dlsym(h, "Z3_mk_bvugt")
   _p_mk_bvsle = dlsym(h, "Z3_mk_bvsle")
   _p_mk_bvslt = dlsym(h, "Z3_mk_bvslt")
   _p_mk_bvsge = dlsym(h, "Z3_mk_bvsge")
   _p_mk_bvsgt = dlsym(h, "Z3_mk_bvsgt")
   _p_solver_assert = dlsym(h, "Z3_solver_assert")
   _p_solver_check = dlsym(h, "Z3_solver_check")
   _p_solver_from_string = dlsym(h, "Z3_solver_from_string")
   _p_solver_get_model = dlsym(h, "Z3_solver_get_model")
   _p_model_inc_ref = dlsym(h, "Z3_model_inc_ref")
   _p_model_dec_ref = dlsym(h, "Z3_model_dec_ref")
   _p_model_eval = dlsym(h, "Z3_model_eval")
   _p_get_numeral_uint64 = dlsym(h, "Z3_get_numeral_uint64")
   _p_mk_numeral = dlsym(h, "Z3_mk_numeral")
   _p_mk_not = dlsym(h, "Z3_mk_not")
   _p_mk_params = dlsym(h, "Z3_mk_params")
   _p_params_inc_ref = dlsym(h, "Z3_params_inc_ref")
   _p_params_dec_ref = dlsym(h, "Z3_params_dec_ref")
   _p_params_set_uint = dlsym(h, "Z3_params_set_uint")
   _p_params_set_bool = dlsym(h, "Z3_params_set_bool")
   _p_solver_set_params = dlsym(h, "Z3_solver_set_params")
   _p_mk_tactic = dlsym(h, "Z3_mk_tactic")
   _p_tactic_inc_ref = dlsym(h, "Z3_tactic_inc_ref")
   _p_tactic_dec_ref = dlsym(h, "Z3_tactic_dec_ref")
   _p_tactic_and_then = dlsym(h, "Z3_tactic_and_then")
   _p_tactic_try_for = dlsym(h, "Z3_tactic_try_for")
   _p_tactic_using_params = dlsym(h, "Z3_tactic_using_params")
   _p_mk_solver_from_tactic = dlsym(h, "Z3_mk_solver_from_tactic")
   _p_get_app_decl = dlsym(h, "Z3_get_app_decl")
   _p_ast_to_string = dlsym(h, "Z3_ast_to_string")
   _p_model_get_const_interp = dlsym(h, "Z3_model_get_const_interp")
   _p_parser_ctx_new = dlsym(h, "Z3_mk_parser_context")
   _p_parser_ctx_inc_ref = dlsym(h, "Z3_parser_context_inc_ref")
   _p_parser_ctx_del = dlsym(h, "Z3_parser_context_dec_ref")
   _p_parser_ctx_add_decl = dlsym(h, "Z3_parser_context_add_decl")
   _p_parser_ctx_from_string = dlsym(h, "Z3_parser_context_from_string")
   _p_ast_vector_size = dlsym(h, "Z3_ast_vector_size")
   _p_ast_vector_get = dlsym(h, "Z3_ast_vector_get")
   _p_ast_vector_inc_ref = dlsym(h, "Z3_ast_vector_inc_ref")
   _p_ast_vector_dec_ref = dlsym(h, "Z3_ast_vector_dec_ref")
   true
}

fn z3_available(): bool {
   "Return true when the Z3 shared library can be loaded."
   _load()
}

fn z3_global_param_set(str: name, str: value): bool {
   "Set a Z3 global parameter, such as `timeout`, using the native Z3 API."
   if(!_load() || !_p_global_param_set){ return false }
   call2(_p_global_param_set, cstr(name), cstr(value))
   true
}

fn z3_global_timeout_ms(any: timeout_ms): bool {
   "Set Z3's process-wide timeout in milliseconds for subsequently created contexts/solvers."
   if(timeout_ms <= 0){ return false }
   z3_global_param_set("timeout", to_str(timeout_ms))
}

fn z3_version(): any {
   "Return Z3 version as [major, minor, build, revision], or nil if unavailable."
   if(!_load()){ return nil }
   def a = malloc(4)
   if(!a){ return nil }
   def b = malloc(4)
   if(!b){ free(a) return nil }
   def c = malloc(4)
   if(!c){ free(a) free(b) return nil }
   def d = malloc(4)
   if(!d){ free(a) free(b) free(c) return nil }
   store32(a,0,0) store32(b,0,0) store32(c,0,0) store32(d,0,0)
   call4_ptr_ptr_ptr_ptr_void(_p_get_version, a, b, c, d)
   def out = [load32(a,0), load32(b,0), load32(c,0), load32(d,0)]
   free(a) free(b) free(c) free(d)
   out
}

fn z3_version_str(): str {
   "Returns Z3 version as a human-readable string like `4.12.2.0`."
   def v = z3_version()
   if(v == nil){ return "" }
   if(v.len < 4){ return "" }
   to_str(v.get(0)) + "." + to_str(v.get(1)) + "." + to_str(v.get(2)) + "." + to_str(v.get(3))
}

fn z3_ctx_new(): any {
   "Create a Z3 context handle, or 0 when Z3 is unavailable."
   if(!_load()){ return 0 }
   def cfg = call0_ptr(_p_mk_config)
   def ctx = call1_ptr(_p_mk_context, cfg)
   call1(_p_del_config, cfg)
   ctx
}

fn z3_ctx_del(any: ctx): any {
   "Destroy a Z3 context handle created by z3_ctx_new."
   if(!_load() || !_is_handle(ctx)){ return nil }
   call1(_p_del_context, ctx)
}

fn z3_ctx_last_error(any: ctx): str {
   "Returns last Z3 error string for `ctx`, or empty string if none."
   if(!_load() || !_is_handle(ctx)){ return "" }
   def code = to_int(call1(_p_get_error_code, ctx))
   if(code == 0){ return "" }
   def msg_raw = call2_ptr(_p_get_error_msg, ctx, _ffi_int(code))
   msg_raw ? str.cstr_to_str(__untag(msg_raw)) : ""
}

fn z3_solver_new(any: ctx): any {
   "Create and retain a Z3 solver for context `ctx`."
   if(!_load() || !_is_handle(ctx)){ return 0 }
   def s = call1_ptr(_p_mk_solver, ctx)
   call2(_p_solver_inc_ref, ctx, s)
   s
}

fn z3_solver_new_for_logic(any: ctx, str: logic): any {
   "Create and retain a Z3 solver specialized for `logic` such as `QF_BV`."
   if(!_load() || !_is_handle(ctx)){ return 0 }
   def s = call2_ptr(_p_mk_solver_for_logic, ctx, z3_sym(ctx, logic))
   if(_is_handle(s)){ call2(_p_solver_inc_ref, ctx, s) }
   s
}

fn z3_tactic_new(any: ctx, str: name): any {
   "Create and retain a Z3 tactic by name, such as `simplify`, `bit-blast`, `sat`, or `qfbv`."
   if(!_load() || !_is_handle(ctx) || !_p_mk_tactic){ return 0 }
   def t = call2_ptr(_p_mk_tactic, ctx, cstr(name))
   if(_is_handle(t) && _p_tactic_inc_ref){ call2(_p_tactic_inc_ref, ctx, t) }
   t
}

fn z3_tactic_del(any: ctx, any: tactic): any {
   "Release a Z3 tactic handle created by z3_tactic_new."
   if(!_load() || !_is_handle(ctx) || !_is_handle(tactic) || !_p_tactic_dec_ref){ return nil }
   call2(_p_tactic_dec_ref, ctx, tactic)
}

fn z3_tactic_and_then(any: ctx, any: first, any: second): any {
   "Compose two Z3 tactics so `second` runs after `first`."
   if(!_load() || !_is_handle(ctx) || !_is_handle(first) || !_is_handle(second)){ return 0 }
   def t = call3_ptr(_p_tactic_and_then, ctx, first, second)
   if(_is_handle(t) && _p_tactic_inc_ref){ call2(_p_tactic_inc_ref, ctx, t) }
   t
}

fn z3_tactic_try_for(any: ctx, any: tactic, any: timeout_ms): any {
   "Wrap a tactic with Z3's native tactic timeout."
   if(!_load() || !_is_handle(ctx) || !_is_handle(tactic) || timeout_ms <= 0){ return tactic }
   def t = call3_ptr_ptr_u32(_p_tactic_try_for, ctx, tactic, int(timeout_ms))
   if(_is_handle(t) && _p_tactic_inc_ref){ call2(_p_tactic_inc_ref, ctx, t) }
   t
}

fn z3_tactic_using_params(any: ctx, any: tactic, any: params): any {
   "Return a tactic configured with a Z3 params handle."
   if(!_load() || !_is_handle(ctx) || !_is_handle(tactic) || !_is_handle(params)){ return 0 }
   def t = call3_ptr(_p_tactic_using_params, ctx, tactic, params)
   if(_is_handle(t) && _p_tactic_inc_ref){ call2(_p_tactic_inc_ref, ctx, t) }
   t
}

fn z3_solver_new_from_tactic(any: ctx, any: tactic): any {
   "Create and retain a solver backed by a Z3 tactic."
   if(!_load() || !_is_handle(ctx) || !_is_handle(tactic) || !_p_mk_solver_from_tactic){ return 0 }
   def s = call2_ptr(_p_mk_solver_from_tactic, ctx, tactic)
   if(_is_handle(s)){ call2(_p_solver_inc_ref, ctx, s) }
   s
}

fn z3_solver_del(any: ctx, any: solver): any {
   "Release a Z3 solver created by z3_solver_new."
   if(!_load() || !_is_handle(ctx) || !_is_handle(solver)){ return nil }
   call2(_p_solver_dec_ref, ctx, solver)
}

fn z3_solver_from_string(any: ctx, any: solver, str: script): any {
   "Parse SMT-LIB2 commands in `script` and assert them into `solver`."
   if(!_load() || !_is_handle(ctx) || !_is_handle(solver)){ return nil }
   call3(_p_solver_from_string, ctx, solver, cstr(script))
}

fn z3_bv_sort(any: ctx, any: bits): any {
   "Create a Z3 bitvector sort of `bits` width."
   call2_ptr_u32(_p_mk_bv_sort, ctx, int(bits))
}

fn z3_int_sort(any: ctx): any {
   "Create a Z3 integer sort."
   call1_ptr(_p_mk_int_sort, ctx)
}

fn z3_sym(any: ctx, str: s): any {
   "Create a Z3 string symbol."
   call2_ptr(_p_mk_string_symbol, ctx, cstr(s))
}

fn z3_bv_const(any: ctx, str: name, any: bits): any {
   "Create a named bitvector constant AST."
   def sym = z3_sym(ctx, name)
   def sort = z3_bv_sort(ctx, bits)
   call3_ptr(_p_mk_const, ctx, sym, sort)
}

fn z3_int_const(any: ctx, str: name): any {
   "Create a named integer constant AST."
   call3_ptr(_p_mk_const, ctx, z3_sym(ctx, name), z3_int_sort(ctx))
}

fn z3_int_val(any: ctx, any: v): any {
   "Create an integer numeral AST."
   call3_ptr(_p_mk_numeral, ctx, cstr(to_str(v)), z3_int_sort(ctx))
}

fn z3_bv_u64(any: ctx, any: v, any: bits): any {
   "Create a bitvector numeral AST from an unsigned 64-bit value."
   def sort = z3_bv_sort(ctx, bits)
   call3_ptr_u64_ptr(_p_mk_unsigned_int64, ctx, int(v), sort)
}

fn z3_bvextract(any: ctx, any: hi, any: lo, any: a): any {
   "Extract bits [hi:lo] from bitvector AST `a`."
   call4_ptr(_p_mk_extract, ctx, hi, lo, a)
}

fn z3_bvconcat(any: ctx, any: hi, any: lo): any {
   "Concatenate two bitvector ASTs with `hi` as the high bits."
   call3_ptr(_p_mk_concat, ctx, hi, lo)
}

fn z3_eq(any: ctx, any: a, any: b): any {
   "Create an equality AST."
   call3_ptr(_p_mk_eq, ctx, a, b)
}

fn z3_int_add(any: ctx, list: args): any {
   "Create an n-ary integer addition AST."
   if(args.len == 0){ return z3_int_val(ctx, 0) }
   if(args.len == 1){ return args[0] }
   def p = _ast_array(args)
   if(!p){ return 0 }
   def out = call3_ptr_u32_ptr(_p_mk_add, ctx, args.len, p)
   free_raw(p)
   out
}

fn z3_int_sub(any: ctx, list: args): any {
   "Create an n-ary integer subtraction AST."
   if(args.len == 0){ return z3_int_val(ctx, 0) }
   if(args.len == 1){ return args[0] }
   def p = _ast_array(args)
   if(!p){ return 0 }
   def out = call3_ptr_u32_ptr(_p_mk_sub, ctx, args.len, p)
   free_raw(p)
   out
}

fn z3_int_mul(any: ctx, list: args): any {
   "Create an n-ary integer multiplication AST."
   if(args.len == 0){ return z3_int_val(ctx, 1) }
   if(args.len == 1){ return args[0] }
   def p = _ast_array(args)
   if(!p){ return 0 }
   def out = call3_ptr_u32_ptr(_p_mk_mul, ctx, args.len, p)
   free_raw(p)
   out
}

fn z3_int_mod(any: ctx, any: a, any: b): any {
   "Create an integer modulo AST."
   call3_ptr(_p_mk_mod, ctx, a, b)
}

fn z3_int_le(any: ctx, any: a, any: b): any {
   "Create an integer <= AST."
   call3_ptr(_p_mk_le, ctx, a, b)
}

fn z3_int_ge(any: ctx, any: a, any: b): any {
   "Create an integer >= AST."
   call3_ptr(_p_mk_ge, ctx, a, b)
}

fn z3_bvadd(any: ctx, any: a, any: b): any {
   "Create a bitvector addition AST."
   call3_ptr(_p_mk_bvadd, ctx, a, b)
}

fn z3_bvsub(any: ctx, any: a, any: b): any {
   "Create a bitvector subtraction AST."
   call3_ptr(_p_mk_bvsub, ctx, a, b)
}

fn z3_bvmul(any: ctx, any: a, any: b): any {
   "Create a bitvector multiplication AST."
   call3_ptr(_p_mk_bvmul, ctx, a, b)
}

fn z3_bvxor(any: ctx, any: a, any: b): any {
   "Create a bitvector xor AST."
   call3_ptr(_p_mk_bvxor, ctx, a, b)
}

fn z3_bvand(any: ctx, any: a, any: b): any {
   "Create a bitvector and AST."
   call3_ptr(_p_mk_bvand, ctx, a, b)
}

fn z3_bvor(any: ctx, any: a, any: b): any {
   "Create a bitvector or AST."
   call3_ptr(_p_mk_bvor, ctx, a, b)
}

fn z3_bvshl(any: ctx, any: a, any: sh): any {
   "Create a bitvector left shift AST."
   call3_ptr(_p_mk_bvshl, ctx, a, sh)
}

fn z3_bvlshr(any: ctx, any: a, any: sh): any {
   "Create a bitvector logical right shift AST."
   call3_ptr(_p_mk_bvlshr, ctx, a, sh)
}

fn z3_bvashr(any: ctx, any: a, any: sh): any {
   "Create a bitvector arithmetic right shift AST."
   call3_ptr(_p_mk_bvashr, ctx, a, sh)
}

fn z3_bvnot(any: ctx, any: a): any {
   "Create a bitvector not AST."
   call2_ptr(_p_mk_bvnot, ctx, a)
}

fn z3_bvneg(any: ctx, any: a): any {
   "Create a bitvector negation AST."
   call2_ptr(_p_mk_bvneg, ctx, a)
}

fn z3_zero_extend(any: ctx, any: extra_bits, any: a): any {
   "Create a zero-extension AST that adds `extra_bits` high bits to bitvector `a`."
   call3_ptr_u32_ptr(_p_mk_zero_ext, ctx, int(extra_bits), a)
}

fn z3_sign_extend(any: ctx, any: extra_bits, any: a): any {
   "Create a sign-extension AST that adds `extra_bits` high bits to bitvector `a`."
   call3_ptr_u32_ptr(_p_mk_sign_ext, ctx, int(extra_bits), a)
}

fn _ast_array(list: args): any {
   def n, p = args.len, malloc_raw(max(1, n) * 8)
   if(!p){ return 0 }
   mut i = 0
   while(i < n){
      store64_h(p, args[i], i * 8)
      i += 1
   }
   p
}

fn z3_mk_or(any: ctx, list: args): any {
   "Create an n-ary boolean OR AST from a list of boolean ASTs."
   if(args.len == 0){ return 0 }
   if(args.len == 1){ return args[0] }
   def p = _ast_array(args)
   if(!p){ return 0 }
   def out = call3_ptr_u32_ptr(_p_mk_or, ctx, args.len, p)
   free_raw(p)
   out
}

fn z3_mk_and(any: ctx, list: args): any {
   "Create an n-ary boolean AND AST from a list of boolean ASTs."
   if(args.len == 0){ return 0 }
   if(args.len == 1){ return args[0] }
   def p = _ast_array(args)
   if(!p){ return 0 }
   def out = call3_ptr_u32_ptr(_p_mk_and, ctx, args.len, p)
   free_raw(p)
   out
}

fn z3_bvudiv(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector division AST."
   call3_ptr(_p_mk_bvudiv, ctx, a, b)
}

fn z3_bvsdiv(any: ctx, any: a, any: b): any {
   "Create a signed bitvector division AST."
   call3_ptr(_p_mk_bvsdiv, ctx, a, b)
}

fn z3_bvurem(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector remainder AST."
   call3_ptr(_p_mk_bvurem, ctx, a, b)
}

fn z3_bvsrem(any: ctx, any: a, any: b): any {
   "Create a signed bitvector remainder AST."
   call3_ptr(_p_mk_bvsrem, ctx, a, b)
}

fn z3_bvule(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector <= predicate AST."
   call3_ptr(_p_mk_bvule, ctx, a, b)
}

fn z3_bvult(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector < predicate AST."
   call3_ptr(_p_mk_bvult, ctx, a, b)
}

fn z3_bvuge(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector >= predicate AST."
   call3_ptr(_p_mk_bvuge, ctx, a, b)
}

fn z3_bvugt(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector > predicate AST."
   call3_ptr(_p_mk_bvugt, ctx, a, b)
}

fn z3_bvsle(any: ctx, any: a, any: b): any {
   "Create a signed bitvector <= predicate AST."
   call3_ptr(_p_mk_bvsle, ctx, a, b)
}

fn z3_bvslt(any: ctx, any: a, any: b): any {
   "Create a signed bitvector < predicate AST."
   call3_ptr(_p_mk_bvslt, ctx, a, b)
}

fn z3_bvsge(any: ctx, any: a, any: b): any {
   "Create a signed bitvector >= predicate AST."
   call3_ptr(_p_mk_bvsge, ctx, a, b)
}

fn z3_bvsgt(any: ctx, any: a, any: b): any {
   "Create a signed bitvector > predicate AST."
   call3_ptr(_p_mk_bvsgt, ctx, a, b)
}

fn z3_solver_assert(any: ctx, any: solver, any: ast): any {
   "Assert an AST into a Z3 solver."
   call3(_p_solver_assert, ctx, solver, ast)
}

fn z3_solver_check(any: ctx, any: solver): bool {
   "Return true when Z3 reports SAT for `solver`."
   def r = call2(_p_solver_check, ctx, solver)
   r == 1
}

fn z3_model_eval_u64(any: ctx, any: solver, any: ast): any {
   "Evaluate a bitvector AST as an unsigned 64-bit integer in the current model."
   if(!z3_solver_check(ctx, solver)){ return nil }
   def m = call2_ptr(_p_solver_get_model, ctx, solver)
   if(!_is_handle(m)){ return nil }
   call2(_p_model_inc_ref, ctx, m)
   def out_ptr = malloc_raw(8)
   if(!out_ptr){
      call2(_p_model_dec_ref, ctx, m)
      return nil
   }
   store64_h(out_ptr, 0, 0)
   def ok = call5(_p_model_eval, ctx, m, ast, _ffi_int(1), out_ptr)
   if(ok == 0){
      call2(_p_model_dec_ref, ctx, m)
      free_raw(out_ptr)
      return nil
   }
   def aval = load64_h(out_ptr, 0)
   free_raw(out_ptr)
   def u64_ptr = malloc_raw(8)
   if(!u64_ptr){
      call2(_p_model_dec_ref, ctx, m)
      return nil
   }
   store64(u64_ptr, 0, 0)
   def ok2 = call3(_p_get_numeral_uint64, ctx, aval, u64_ptr)
   def u64v = load64_h(u64_ptr, 0)
   free_raw(u64_ptr)
   call2(_p_model_dec_ref, ctx, m)
   ok2 ? u64v : nil
}

fn ctx_new(): any {
   "Create a Z3 context handle."
   z3_ctx_new()
}

fn ctx_del(any: ctx): any {
   "Destroy a Z3 context handle."
   z3_ctx_del(ctx)
}

fn ctx_last_error(any: ctx): str {
   "Return the last Z3 error string for a context."
   z3_ctx_last_error(ctx)
}

fn global_param_set(str: name, str: value): bool {
   "Set a Z3 global parameter through the native API."
   z3_global_param_set(name, value)
}

fn global_timeout_ms(any: timeout_ms): bool {
   "Set Z3's process-wide timeout in milliseconds."
   z3_global_timeout_ms(timeout_ms)
}

fn solver_new(any: ctx): any {
   "Create a Z3 solver for context `ctx`."
   z3_solver_new(ctx)
}

fn solver_new_for_logic(any: ctx, str: logic): any {
   "Create a Z3 solver specialized for `logic`."
   z3_solver_new_for_logic(ctx, logic)
}

fn solver_new_qfbv(any: ctx): any {
   "Create a Z3 solver specialized for quantifier-free bitvectors."
   z3_solver_new_for_logic(ctx, "QF_BV")
}

fn tactic_new(any: ctx, str: name): any {
   "Create a Z3 tactic by name."
   z3_tactic_new(ctx, name)
}

fn tactic_del(any: ctx, any: tactic): any {
   "Release a tactic handle."
   z3_tactic_del(ctx, tactic)
}

fn tactic_and_then(any: ctx, any: first, any: second): any {
   "Compose two Z3 tactics."
   z3_tactic_and_then(ctx, first, second)
}

fn tactic_chain(any: ctx, list: names): any {
   "Compose a list of named Z3 tactics in order."
   if(names.len == 0){ return 0 }
   mut current = tactic_new(ctx, names[0])
   if(!_is_handle(current)){ return 0 }
   mut i = 1
   while(i < names.len){
      def next = tactic_new(ctx, names[i])
      if(!_is_handle(next)){
         tactic_del(ctx, current)
         return 0
      }
      def joined = tactic_and_then(ctx, current, next)
      tactic_del(ctx, current)
      tactic_del(ctx, next)
      if(!_is_handle(joined)){ return 0 }
      current = joined
      i += 1
   }
   current
}

fn tactic_try_for(any: ctx, any: tactic, any: timeout_ms): any {
   "Return a tactic wrapped in Z3's native timeout."
   z3_tactic_try_for(ctx, tactic, timeout_ms)
}

fn tactic_using_params(any: ctx, any: tactic, any: params): any {
   "Return a tactic configured with a Z3 params handle."
   z3_tactic_using_params(ctx, tactic, params)
}

fn solver_new_from_tactic(any: ctx, any: tactic): any {
   "Create a solver backed by a Z3 tactic."
   z3_solver_new_from_tactic(ctx, tactic)
}

fn solver_new_tactic_named(any: ctx, str: name, any: timeout_ms=0): any {
   "Create a solver backed by a named Z3 tactic, optionally wrapped in a native timeout."
   mut tactic = tactic_new(ctx, name)
   if(!_is_handle(tactic)){ return 0 }
   if(timeout_ms > 0){
      def timed = tactic_try_for(ctx, tactic, timeout_ms)
      if(_is_handle(timed) && timed != tactic){
         tactic_del(ctx, tactic)
         tactic = timed
      }
   }
   def solver = solver_new_from_tactic(ctx, tactic)
   tactic_del(ctx, tactic)
   solver
}

fn solver_new_tactic_chain(any: ctx, list: names, any: timeout_ms=0): any {
   "Create a solver backed by a composed Z3 tactic chain."
   mut tactic = tactic_chain(ctx, names)
   if(!_is_handle(tactic)){ return 0 }
   if(timeout_ms > 0){
      def timed = tactic_try_for(ctx, tactic, timeout_ms)
      if(_is_handle(timed) && timed != tactic){
         tactic_del(ctx, tactic)
         tactic = timed
      }
   }
   def solver = solver_new_from_tactic(ctx, tactic)
   tactic_del(ctx, tactic)
   solver
}

fn solver_new_qfbv_tactic(any: ctx, any: timeout_ms=0): any {
   "Create a QF_BV tactic solver, optionally wrapped in a native Z3 tactic timeout."
   solver_new_tactic_named(ctx, "qfbv", timeout_ms)
}

fn _params_new(any: ctx): any {
   def p = call1_ptr(_p_mk_params, ctx)
   if(_is_handle(p)){ call2(_p_params_inc_ref, ctx, p) }
   p
}

fn _params_del(any: ctx, any: p): any { if(_is_handle(p)){ call2(_p_params_dec_ref, ctx, p) } }

fn _params_uint(any: ctx, any: p, str: name, any: value): any { if(_is_handle(p)){ call4(_p_params_set_uint, ctx, p, z3_sym(ctx, name), int(value)) } }

fn _params_bool(any: ctx, any: p, str: name, any: value): any { if(_is_handle(p) && _p_params_set_bool){ call4(_p_params_set_bool, ctx, p, z3_sym(ctx, name), value ? 1 : 0) } }

fn solver_new_qfbv_sls(any: ctx, any: timeout_ms=0, any: seed=0, any: max_rounds=128): any {
   "Create a configured stochastic local-search solver for satisfiable QF_BV problems."
   mut tactic = tactic_new(ctx, "qfbv-sls")
   if(!_is_handle(tactic)){ return 0 }
   def params = _params_new(ctx)
   if(_is_handle(params)){
      _params_uint(ctx, params, "max_rounds", max_rounds)
      _params_uint(ctx, params, "max_steps", 4294967295)
      _params_uint(ctx, params, "max_restarts", 4294967295)
      _params_uint(ctx, params, "restart_base", 32)
      _params_uint(ctx, params, "random_seed", seed)
      _params_bool(ctx, params, "restart_init", true)
      def tuned = tactic_using_params(ctx, tactic, params)
      tactic_del(ctx, tactic)
      tactic = tuned
      _params_del(ctx, params)
      if(!_is_handle(tactic)){ return 0 }
   }
   if(timeout_ms > 0){
      def timed = tactic_try_for(ctx, tactic, timeout_ms)
      if(_is_handle(timed) && timed != tactic){
         tactic_del(ctx, tactic)
         tactic = timed
      }
   }
   def solver = solver_new_from_tactic(ctx, tactic)
   tactic_del(ctx, tactic)
   solver
}

fn solver_del(any: ctx, any: s): any {
   "Release a solver created by solver_new."
   z3_solver_del(ctx, s)
}

fn solver_from_string(any: ctx, any: s, str: script): any {
   "Parse SMT-LIB2 commands in `script` and assert them into solver `s`."
   z3_solver_from_string(ctx, s, script)
}

fn bv_sort(any: ctx, any: bits): any {
   "Create a bitvector sort of `bits` width."
   z3_bv_sort(ctx, bits)
}

fn int_sort(any: ctx): any {
   "Create the unbounded integer sort."
   z3_int_sort(ctx)
}

fn sym(any: ctx, str: name): any {
   "Create a Z3 string symbol."
   z3_sym(ctx, name)
}

fn bv_const(any: ctx, str: name, any: bits): any {
   "Create a named bitvector constant."
   z3_bv_const(ctx, name, bits)
}

fn int_const(any: ctx, str: name): any {
   "Create a named integer constant."
   z3_int_const(ctx, name)
}

fn int_val(any: ctx, any: v): any {
   "Create an integer numeral."
   z3_int_val(ctx, v)
}

fn bv_u64(any: ctx, any: v, any: bits): any {
   "Create a bitvector numeral from an unsigned 64-bit value."
   z3_bv_u64(ctx, v, bits)
}

fn bv_extract(any: ctx, any: hi, any: lo, any: a): any {
   "Extract bits [hi:lo] from bitvector AST `a`."
   z3_bvextract(ctx, hi, lo, a)
}

fn bvconcat(any: ctx, any: hi, any: lo): any {
   "Concatenate two bitvector ASTs with `hi` as the high bits."
   z3_bvconcat(ctx, hi, lo)
}

fn _hex_digit(int: c): int {
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   c - 87
}

fn _dec_mul16_add(str: dec, int: add): str {
   def n = dec.len
   mut carry = add
   mut result = ""
   mut i = n - 1
   while(i >= 0){
      def prod = (load8(dec, i) - 48) * 16 + carry
      result = str.chr(48 + prod % 10) + result
      carry = prod / 10
      i = i - 1
   }
   while(carry > 0){
      result = str.chr(48 + carry % 10) + result
      carry = carry / 10
   }
   result
}

fn _hex_to_dec(str: hex): str {
   def n = hex.len
   mut dec = "0"
   mut i = 0
   while(i < n){
      dec = _dec_mul16_add(dec, _hex_digit(load8(hex, i)))
      i += 1
   }
   dec
}

fn bv_hex(any: ctx, str: hex, any: bits): any {
   "Create a bitvector constant from a hex string(no prefix, e.g. 'deadbeef').
   Works for any bit width including 128-bit and wider."
   def sort = z3_bv_sort(ctx, bits)
   call3_ptr(_p_mk_numeral, ctx, cstr(_hex_to_dec(hex)), sort)
}

fn mk_eq(any: ctx, any: a, any: b): any {
   "Create an equality AST."
   z3_eq(ctx, a, b)
}

fn mk_not(any: ctx, any: a): any {
   "Build the negation ¬a as a Z3 AST."
   call2_ptr(_p_mk_not, ctx, a)
}

fn int_add(any: ctx, list: args): any { z3_int_add(ctx, args) }

fn int_sub(any: ctx, list: args): any { z3_int_sub(ctx, args) }

fn int_mul(any: ctx, list: args): any { z3_int_mul(ctx, args) }

fn int_mod(any: ctx, any: a, any: b): any { z3_int_mod(ctx, a, b) }

fn int_le(any: ctx, any: a, any: b): any { z3_int_le(ctx, a, b) }

fn int_ge(any: ctx, any: a, any: b): any { z3_int_ge(ctx, a, b) }

fn mk_neq(any: ctx, any: a, any: b): any {
   "Create a disequality AST."
   mk_not(ctx, mk_eq(ctx, a, b))
}

fn mk_or(any: ctx, list: args): any {
   "Create an n-ary boolean OR AST from a list of boolean ASTs."
   z3_mk_or(ctx, args)
}

fn mk_and(any: ctx, list: args): any {
   "Create an n-ary boolean AND AST from a list of boolean ASTs."
   z3_mk_and(ctx, args)
}

fn bvadd(any: ctx, any: a, any: b): any {
   "Create a bitvector addition AST."
   z3_bvadd(ctx, a, b)
}

fn bvsub(any: ctx, any: a, any: b): any {
   "Create a bitvector subtraction AST."
   z3_bvsub(ctx, a, b)
}

fn bvmul(any: ctx, any: a, any: b): any {
   "Create a bitvector multiplication AST."
   z3_bvmul(ctx, a, b)
}

fn bvxor(any: ctx, any: a, any: b): any {
   "Create a bitvector xor AST."
   z3_bvxor(ctx, a, b)
}

fn bvand(any: ctx, any: a, any: b): any {
   "Create a bitvector and AST."
   z3_bvand(ctx, a, b)
}

fn bvor(any: ctx, any: a, any: b): any {
   "Create a bitvector or AST."
   z3_bvor(ctx, a, b)
}

fn bvshl(any: ctx, any: a, any: sh): any {
   "Create a bitvector left shift AST."
   z3_bvshl(ctx, a, sh)
}

fn bvlshr(any: ctx, any: a, any: sh): any {
   "Create a bitvector logical right shift AST."
   z3_bvlshr(ctx, a, sh)
}

fn bvashr(any: ctx, any: a, any: sh): any {
   "Create a bitvector arithmetic right shift AST."
   z3_bvashr(ctx, a, sh)
}

fn bvnot(any: ctx, any: a): any {
   "Create a bitvector not AST."
   z3_bvnot(ctx, a)
}

fn bvneg(any: ctx, any: a): any {
   "Create a bitvector negation AST."
   z3_bvneg(ctx, a)
}

fn bvzext(any: ctx, any: a, any: extra_bits): any {
   "Zero-extend bitvector `a` by `extra_bits` high bits."
   z3_zero_extend(ctx, extra_bits, a)
}

fn bvsext(any: ctx, any: a, any: extra_bits): any {
   "Sign-extend bitvector `a` by `extra_bits` high bits."
   z3_sign_extend(ctx, extra_bits, a)
}

fn bvudiv(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector division AST."
   z3_bvudiv(ctx, a, b)
}

fn bvsdiv(any: ctx, any: a, any: b): any {
   "Create a signed bitvector division AST."
   z3_bvsdiv(ctx, a, b)
}

fn bvurem(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector remainder AST."
   z3_bvurem(ctx, a, b)
}

fn bvsrem(any: ctx, any: a, any: b): any {
   "Create a signed bitvector remainder AST."
   z3_bvsrem(ctx, a, b)
}

fn bvule(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector <= predicate AST."
   z3_bvule(ctx, a, b)
}

fn bvult(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector < predicate AST."
   z3_bvult(ctx, a, b)
}

fn bvuge(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector >= predicate AST."
   z3_bvuge(ctx, a, b)
}

fn bvugt(any: ctx, any: a, any: b): any {
   "Create an unsigned bitvector > predicate AST."
   z3_bvugt(ctx, a, b)
}

fn bvsle(any: ctx, any: a, any: b): any {
   "Create a signed bitvector <= predicate AST."
   z3_bvsle(ctx, a, b)
}

fn bvslt(any: ctx, any: a, any: b): any {
   "Create a signed bitvector < predicate AST."
   z3_bvslt(ctx, a, b)
}

fn bvsge(any: ctx, any: a, any: b): any {
   "Create a signed bitvector >= predicate AST."
   z3_bvsge(ctx, a, b)
}

fn bvsgt(any: ctx, any: a, any: b): any {
   "Create a signed bitvector > predicate AST."
   z3_bvsgt(ctx, a, b)
}

fn solver_assert(any: ctx, any: s, any: ast): any {
   "Assert an AST into a solver."
   z3_solver_assert(ctx, s, ast)
}

fn solver_check(any: ctx, any: s): bool {
   "Return true when solver `s` is satisfiable."
   z3_solver_check(ctx, s)
}

fn solver_check_result(any: ctx, any: s): int {
   "Return SAT(1), UNSAT(-1), or UNKNOWN(0) for solver `s`."
   def r = call2(_p_solver_check, ctx, s)
   r == 1 ? SAT : ((r == -1 || r == 0xffffffff) ? UNSAT : UNKNOWN)
}

fn solver_set_timeout_ms(any: ctx, any: s, any: timeout_ms): bool {
   "Set a per-check Z3 timeout in milliseconds for solver `s`."
   if(!_load() || !_is_handle(ctx) || !_is_handle(s) || timeout_ms <= 0){ return false }
   def p = call1_ptr(_p_mk_params, ctx)
   if(!_is_handle(p)){ return false }
   call2(_p_params_inc_ref, ctx, p)
   call4(_p_params_set_uint, ctx, p, z3_sym(ctx, "timeout"), int(timeout_ms))
   call3(_p_solver_set_params, ctx, s, p)
   call2(_p_params_dec_ref, ctx, p)
   true
}

fn model_eval_u64(any: ctx, any: s, any: ast): any {
   "Evaluate a bitvector AST as an unsigned 64-bit integer."
   z3_model_eval_u64(ctx, s, ast)
}

fn const_decl(any: ctx, any: ast): any {
   "Return the func_decl for a bitvector constant AST(via Z3_get_app_decl)."
   call2_ptr(_p_get_app_decl, ctx, ast)
}

fn _bin_to_hex(str: b): str {
   def n = b.len
   def rem = n % 4
   mut s = b
   if(rem != 0){
      mut j = 0
      while(j < 4 - rem){ s = "0" + s j += 1 }
   }
   def hex_chars = "0123456789abcdef"
   mut out = str.Builder(max(16, (s.len / 4) + 8))
   mut i = 0
   def total = s.len
   while(i < total){
      def nibble = (load8(s,i)-48)*8 + (load8(s,i+1)-48)*4 + (load8(s,i+2)-48)*2 + (load8(s,i+3)-48)
      out = str.builder_append(out, str.chr(load8(hex_chars, nibble)))
      i += 4
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn _slice_from(str: s, int: start): str {
   mut out = str.Builder(max(0, s.len - start))
   mut i = start
   while(i < s.len){
      out = str.builder_append(out, str.chr(load8(s, i)))
      i += 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn _ast_to_hex(any: ctx, any: ast): str {
   if(!_is_handle(ast)){ return "" }
   def s_raw = call2_ptr(_p_ast_to_string, ctx, ast)
   if(!s_raw){ return "" }
   def s = str.cstr_to_str(__untag(s_raw))
   if(s.len >= 2 && load8(s, 0) == 35 && load8(s, 1) == 120){ return _slice_from(s, 2) }
   if(s.len >= 2 && load8(s, 0) == 35 && load8(s, 1) == 98){ return _bin_to_hex(_slice_from(s, 2)) }
   s
}

fn model_eval_hex(any: ctx, any: solver, any: ast): any {
   "Evaluate an AST in the current model and return its value as a hex string.
   Works for any bit width(8, 16, 32, 64, 128, ...)."
   if(!z3_solver_check(ctx, solver)){ return nil }
   def m = call2_ptr(_p_solver_get_model, ctx, solver)
   if(!_is_handle(m)){ return nil }
   call2(_p_model_inc_ref, ctx, m)
   def out_ptr = malloc_raw(8)
   if(!out_ptr){
      call2(_p_model_dec_ref, ctx, m)
      return nil
   }
   store64_h(out_ptr, 0, 0)
   def ok = call5(_p_model_eval, ctx, m, ast, _ffi_int(1), out_ptr)
   if(ok == 0){
      call2(_p_model_dec_ref, ctx, m)
      free_raw(out_ptr)
      return nil
   }
   def aval = load64_h(out_ptr, 0)
   free_raw(out_ptr)
   call2(_p_model_dec_ref, ctx, m)
   _ast_to_hex(ctx, aval)
}

fn hex_width(any: hex, any: bits): any {
   "Normalize a hex string to the low `bits` bits, padded to that exact nibble width."
   if(hex == nil){ return nil }
   mut h = str.lower(str.strip(to_str(hex)))
   if(h.len >= 2 && load8(h, 0) == 35 && load8(h, 1) == 120){
      h = _slice_from(h, 2)
   } elif(h.len >= 2 && load8(h, 0) == 35 && load8(h, 1) == 98){
      h = _bin_to_hex(_slice_from(h, 2))
   } elif(h.len >= 2 && load8(h, 0) == 48 && load8(h, 1) == 120){
      h = _slice_from(h, 2)
   }
   def width = (int(bits) + 3) / 4
   if(width <= 0){ return h }
   if(h.len > width){ h = _slice_from(h, h.len - width) }
   while(h.len < width){ h = "0" + h }
   h
}

fn model_eval_hex_width(any: ctx, any: solver, any: ast, any: bits): any {
   "Evaluate an AST as hex and normalize the result to `bits` width."
   hex_width(model_eval_hex(ctx, solver, ast), bits)
}

fn bv_u8(any: ctx, any: v): any {
   "Create an 8-bit bitvector constant from integer `v`."
   bv_u64(ctx, v & 255, 8)
}

fn bv_u16(any: ctx, any: v): any {
   "Create a 16-bit bitvector constant from integer `v`."
   bv_u64(ctx, v & 65535, 16)
}

fn bv_u32(any: ctx, any: v): any {
   "Create a 32-bit bitvector constant from integer `v`."
   bv_u64(ctx, v & 4294967295, 32)
}

fn bv_bytes(any: ctx, str: prefix, int: n): list {
   "Create `n` symbolic 8-bit variables named `prefix0`, `prefix1`, ..."
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(bv_const(ctx, to_str(prefix) + to_str(i), 8))
      i += 1
   }
   out
}

fn bv_words(any: ctx, str: prefix, int: n, any: bits=32): list {
   "Create `n` symbolic bitvector variables named `prefix0`, `prefix1`, ... with `bits` width."
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(bv_const(ctx, to_str(prefix) + to_str(i), bits))
      i += 1
   }
   out
}

fn _bytes_input_len(any: values): int {
   if(is_str(values) || is_bytes(values)){ return values.len }
   if(is_list(values)){ return values.len }
   0
}

fn _bytes_input_at(any: values, int: i): int {
   if(is_str(values) || is_bytes(values)){ return load8(values, i) & 255 }
   if(is_list(values)){ return int(values.get(i, 0)) & 255 }
   0
}

fn _bytes_key_at(any: key, int: i): int {
   if(is_int(key)){ return int(key) & 255 }
   def n = _bytes_input_len(key)
   if(n <= 0){ return 0 }
   _bytes_input_at(key, i % n)
}

fn solver_assert_bytes_eq(any: ctx, any: solver, list: xs, any: values): int {
   "Assert a symbolic byte vector equals a string, bytes object, or integer list.
   Returns the number of byte constraints asserted."
   def n = min(xs.len, _bytes_input_len(values))
   mut i = 0
   while(i < n){
      solver_assert(ctx, solver, mk_eq(ctx, xs[i], bv_u8(ctx, _bytes_input_at(values, i))))
      i += 1
   }
   n
}

fn solver_assert_bytes_xor_eq(any: ctx, any: solver, list: xs, any: key, any: values): int {
   "Assert `(xs[i] xor key[i]) == values[i]`.
   `key` may be a scalar byte, string/bytes, or integer list and repeats when shorter than `xs`."
   def n = min(xs.len, _bytes_input_len(values))
   mut i = 0
   while(i < n){
      def lhs = bvxor(ctx, xs[i], bv_u8(ctx, _bytes_key_at(key, i)))
      solver_assert(ctx, solver, mk_eq(ctx, lhs, bv_u8(ctx, _bytes_input_at(values, i))))
      i += 1
   }
   n
}

fn solver_assert_bytes_xor_reduce8(any: ctx, any: solver, list: xs, any: value): int {
   "Assert the xor-reduction of a byte vector equals `value` modulo 256."
   mut acc = bv_u8(ctx, 0)
   mut i = 0
   while(i < xs.len){
      acc = bvxor(ctx, acc, xs[i])
      i += 1
   }
   solver_assert(ctx, solver, mk_eq(ctx, acc, bv_u8(ctx, value)))
   xs.len
}

fn solver_assert_bytes_add_sum8(any: ctx, any: solver, list: xs, any: value): int {
   "Assert the byte-wise sum of a vector equals `value` modulo 256."
   mut acc = bv_u8(ctx, 0)
   mut i = 0
   while(i < xs.len){
      acc = bvadd(ctx, acc, xs[i])
      i += 1
   }
   solver_assert(ctx, solver, mk_eq(ctx, acc, bv_u8(ctx, value)))
   xs.len
}

fn solver_assert_bytes_ascii_range(any: ctx, any: solver, list: xs, any: lo=32, any: hi=126): int {
   "Constrain symbolic bytes to an inclusive ASCII byte range."
   mut low = max(0, int(lo))
   mut high = min(255, int(hi))
   if(low > high){
      def tmp = low
      low = high
      high = tmp
   }
   mut i = 0
   while(i < xs.len){
      solver_assert(ctx, solver, bvuge(ctx, xs[i], bv_u8(ctx, low)))
      solver_assert(ctx, solver, bvule(ctx, xs[i], bv_u8(ctx, high)))
      i += 1
   }
   xs.len
}

fn _bv_shift(any: ctx, any: shift, any: bits): any { bv_u64(ctx, int(shift) % int(bits), bits) }

fn bvrotl(any: ctx, any: a, any: shift, any: bits): any {
   "Rotate bitvector `a` left by constant `shift` within `bits` bits."
   def s = int(shift) % int(bits)
   if(s == 0){ return a }
   def left = bvshl(ctx, a, _bv_shift(ctx, s, bits))
   def right = bvlshr(ctx, a, _bv_shift(ctx, int(bits) - s, bits))
   bvor(ctx, left, right)
}

fn bvrotr(any: ctx, any: a, any: shift, any: bits): any {
   "Rotate bitvector `a` right by constant `shift` within `bits` bits."
   def s = int(shift) % int(bits)
   if(s == 0){ return a }
   def right = bvlshr(ctx, a, _bv_shift(ctx, s, bits))
   def left = bvshl(ctx, a, _bv_shift(ctx, int(bits) - s, bits))
   bvor(ctx, left, right)
}

fn model_eval_bytes(any: ctx, any: solver, list: xs): any {
   "Evaluate a list of 8-bit ASTs in the current model and return byte integers."
   if(!solver_check(ctx, solver)){ return nil }
   mut out = []
   mut i = 0
   while(i < xs.len){
      def v = model_eval_u64(ctx, solver, xs[i])
      if(v == nil){ return nil }
      out = out.append(v & 255)
      i += 1
   }
   out
}

fn model_eval_bytes_hex(any: ctx, any: solver, list: xs): any {
   "Evaluate 8-bit ASTs and return a packed lowercase hex string."
   def bs = model_eval_bytes(ctx, solver, xs)
   if(bs == nil){ return nil }
   mut out = str.Builder(max(16, bs.len * 2))
   mut i = 0
   while(i < bs.len){
      out = str.builder_append(out, str.to_hex(bs[i] & 255, 2))
      i += 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn model_eval_ascii(any: ctx, any: solver, list: xs): any {
   "Evaluate a list of 8-bit ASTs and return an ASCII string."
   def bs = model_eval_bytes(ctx, solver, xs)
   if(bs == nil){ return nil }
   mut out = str.Builder(bs.len)
   mut i = 0
   while(i < bs.len){
      out = str.builder_append(out, str.chr(bs[i]))
      i += 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn model_eval_ascii_checked(any: ctx, any: solver, list: xs, any: lo=0, any: hi=255): any {
   "Evaluate 8-bit ASTs as a string, returning nil if any byte is outside range."
   def bs = model_eval_bytes(ctx, solver, xs)
   if(bs == nil){ return nil }
   mut low = max(0, int(lo))
   mut high = min(255, int(hi))
   if(low > high){
      def tmp = low
      low = high
      high = tmp
   }
   mut out = str.Builder(bs.len)
   mut i = 0
   while(i < bs.len){
      def b = bs[i]
      if(b < low || b > high){
         str.builder_free(out)
         return nil
      }
      out = str.builder_append(out, str.chr(b))
      i += 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn model_eval_decl_bv_hex(any: ctx, any: solver, any: decl, any: bits): any {
   "Evaluate a constant declaration in the current model and return its value as a hex string."
   if(!z3_solver_check(ctx, solver)){ return nil }
   def m = call2_ptr(_p_solver_get_model, ctx, solver)
   if(!_is_handle(m)){ return nil }
   call2(_p_model_inc_ref, ctx, m)
   def val_ast = call3_ptr(_p_model_get_const_interp, ctx, m, decl)
   def result = _is_handle(val_ast) ? _ast_to_hex(ctx, val_ast) : nil
   call2(_p_model_dec_ref, ctx, m)
   result
}

fn solver_assert_decl_not_hex(any: ctx, any: solver, any: ast, str: hex, any: bits): any {
   "Assert that variable `ast` is NOT equal to the given hex value(solution blocking)."
   def val = bv_hex(ctx, hex, bits)
   def neg = mk_not(ctx, z3_eq(ctx, ast, val))
   z3_solver_assert(ctx, solver, neg)
}

fn parser_ctx_new(any: ctx): any {
   "Create a Z3 parser context for incremental SMT-LIB2 parsing with known declarations."
   def pc = call1_ptr(_p_parser_ctx_new, ctx)
   if(_is_handle(pc)){ call2(_p_parser_ctx_inc_ref, ctx, pc) }
   pc
}

fn parser_ctx_free(any: ctx, any: pc): any {
   "Destroy a Z3 parser context."
   call2(_p_parser_ctx_del, ctx, pc)
}

fn parser_ctx_add_decl(any: ctx, any: pc, any: decl): any {
   "Register a func_decl into the parser context so it can be referenced by name."
   call3(_p_parser_ctx_add_decl, ctx, pc, decl)
}

fn parser_ctx_parse(any: ctx, any: pc, str: script): any {
   "Parse SMT-LIB2 `script` with the parser context. Returns a Z3 ast_vector of assertions."
   def vec = call3_ptr(_p_parser_ctx_from_string, ctx, pc, cstr(script))
   if(_is_handle(vec)){ call2(_p_ast_vector_inc_ref, ctx, vec) }
   vec
}

fn solver_assert_ast_vector(any: ctx, any: solver, any: vec): any {
   "Assert all ASTs in a Z3 ast_vector into the solver."
   def sz = call2(_p_ast_vector_size, ctx, vec)
   mut i = 0
   while(i < sz){
      def ast = call3_ptr(_p_ast_vector_get, ctx, vec, i)
      z3_solver_assert(ctx, solver, ast)
      i += 1
   }
}

fn ast_vector_free(any: ctx, any: vec): any {
   "Decrement reference count on a Z3 ast_vector, freeing it."
   call2(_p_ast_vector_dec_ref, ctx, vec)
}

fn session_new(): any {
   "Create a session(context + solver pair) as a [ctx, solver] list."
   def ctx = z3_ctx_new()
   if(!_is_handle(ctx)){ return nil }
   def s = z3_solver_new(ctx)
   if(!_is_handle(s)){ z3_ctx_del(ctx) return nil }
   [ctx, s]
}

fn session_free(any: sess): any {
   "Destroy a session created by session_new."
   if(sess == nil){ return nil }
   def ctx = sess.get(0)
   def s = sess.get(1)
   z3_solver_del(ctx, s)
   z3_ctx_del(ctx)
}

fn session_ctx(any: sess): any {
   "Return the context handle from a session."
   sess.get(0)
}

fn session_solver(any: sess): any {
   "Return the solver handle from a session."
   sess.get(1)
}

fn session_check(any: sess): int {
   "Run Z3_solver_check and return SAT(1), UNSAT(-1), or UNKNOWN(0)."
   def ctx = sess.get(0)
   def s = sess.get(1)
   def r = call2(_p_solver_check, ctx, s)
   r == 1 ? 1 : (r == 0 ? 0 : -1)
}

fn solve_qf_bv_const_hex(str: script, str: name, any: bits): any {
   "One-shot high-level helper: parse full SMT-LIB2 `script`, check satisfiability,
   and return the value of variable `name` (bitvector of `bits` width) as a hex string.
   Returns nil if UNSAT or Z3 is unavailable."
   def sess = session_new()
   if(sess == nil){ return nil }
   def ctx = session_ctx(sess)
   def s = session_solver(sess)
   z3_solver_from_string(ctx, s, script)
   if(session_check(sess) != SAT){
      session_free(sess)
      return nil
   }
   def x = bv_const(ctx, name, bits)
   def result = model_eval_hex(ctx, s, x)
   session_free(sess)
   result
}

fn solve_qf_bv_bytes(str: script, str: prefix, int: n): any {
   "One-shot helper for byte-vector SMT-LIB problems; returns model bytes."
   def sess = session_new()
   if(sess == nil){ return nil }
   def ctx = session_ctx(sess)
   def s = session_solver(sess)
   z3_solver_from_string(ctx, s, script)
   if(session_check(sess) != SAT){
      session_free(sess)
      return nil
   }
   def xs = bv_bytes(ctx, prefix, n)
   def result = model_eval_bytes(ctx, s, xs)
   session_free(sess)
   result
}

fn solve_qf_bv_bytes_hex(str: script, str: prefix, int: n): any {
   "One-shot helper for byte-vector SMT-LIB problems; returns packed model hex."
   def sess = session_new()
   if(sess == nil){ return nil }
   def ctx = session_ctx(sess)
   def s = session_solver(sess)
   z3_solver_from_string(ctx, s, script)
   if(session_check(sess) != SAT){
      session_free(sess)
      return nil
   }
   def xs = bv_bytes(ctx, prefix, n)
   def result = model_eval_bytes_hex(ctx, s, xs)
   session_free(sess)
   result
}

fn solve_qf_bv_ascii(str: script, str: prefix, int: n): any {
   "One-shot helper for byte-vector SMT-LIB problems named prefix0..prefixN."
   def sess = session_new()
   if(sess == nil){ return nil }
   def ctx = session_ctx(sess)
   def s = session_solver(sess)
   z3_solver_from_string(ctx, s, script)
   if(session_check(sess) != SAT){
      session_free(sess)
      return nil
   }
   def xs = bv_bytes(ctx, prefix, n)
   def result = model_eval_ascii(ctx, s, xs)
   session_free(sess)
   result
}
