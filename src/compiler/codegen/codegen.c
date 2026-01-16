#include "codegen.h"
#include "std_symbols.h"
#include <alloca.h>
#include <sys/types.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <setjmp.h>

typedef NT_VEC(binding) binding_list;
typedef NT_VEC(char *) nt_str_list;
typedef NT_VEC(char *) str_list;

typedef struct scope {
	NT_VEC(binding) vars;
	NT_VEC(nt_stmt *) defers;
	LLVMBasicBlockRef break_bb;
	LLVMBasicBlockRef continue_bb;
} scope;

static void add_builtins(nt_codegen *cg);
static fun_sig *lookup_fun(nt_codegen *cg, const char *name);
static fun_sig *resolve_overload(nt_codegen *cg, const char *name, size_t argc);
static LLVMValueRef to_bool(nt_codegen *cg, LLVMValueRef v);
static binding *scope_lookup(scope *scopes, size_t depth, const char *name);
static void bind(scope *scopes, size_t depth, const char *name, LLVMValueRef v, nt_stmt *stmt);
static LLVMValueRef gen_expr(nt_codegen *cg, scope *scopes, size_t depth, nt_expr *e);
static void gen_stmt(nt_codegen *cg, scope *scopes, size_t *depth, nt_stmt *s, size_t func_root, bool is_tail);
static void gen_func(nt_codegen *cg, nt_stmt *fn, const char *name, scope *scopes, size_t depth, binding_list *captures);
static LLVMValueRef const_string_ptr(nt_codegen *cg, const char *s, size_t len);
static LLVMValueRef gen_binary(nt_codegen *cg, const char *op, LLVMValueRef l, LLVMValueRef r);
static fun_sig *lookup_use_module_fun(nt_codegen *cg, const char *name, size_t argc);
static const char *resolve_import_alias(nt_codegen *cg, const char *name);

static struct builtin_def {
	const char *name;
	int args;
} builtin_defs[] = {
	{"rt_malloc", 1},
	{"rt_init_str", 2},
	{"rt_free", 1},
	{"rt_realloc", 2}, {"rt_memcpy", 3}, {"rt_memset", 3}, {"rt_memcmp", 3},
	{"rt_load8", 1},
	{"rt_load8_idx", 2},
	{"rt_load16_idx", 2},
	{"rt_load32_idx", 2},
	{"rt_load64_idx", 2},
	{"rt_store8_idx", 3},
	{"rt_store16_idx", 3},
	{"rt_store32_idx", 3},
	{"rt_store64_idx", 3},
	{"rt_sys_read_off", 4},
	{"rt_sys_write_off", 4},
	{"rt_store8", 2},
	{"rt_load16", 1},
	{"rt_store16", 2},
	{"rt_load32", 1},
	{"rt_store32", 2},
	{"rt_load64", 1},
	{"rt_store64", 2},
	{"rt_add", 2},
	{"rt_sub", 2},
	{"rt_mul", 2},
	{"rt_div", 2},
	{"rt_mod", 2},
	{"rt_and", 2},
	{"rt_or", 2},
	{"rt_xor", 2},
	{"rt_shl", 2},
	{"rt_shr", 2},
	{"rt_not", 1},
	{"rt_str_concat", 2},
	{"rt_eq", 2}, {"rt_lt", 2}, {"rt_le", 2}, {"rt_gt", 2}, {"rt_ge", 2},
	{"rt_to_str", 1},
	{"rt_is_int", 1},
	{"rt_is_ptr", 1},
	{"rt_to_int", 1},
	{"rt_ptr_add", 2},
	{"rt_ptr_sub", 2},
	{"rt_from_int", 1},
	{"rt_panic", 1},
	{"rt_exit", 1},
	{"rt_argc", 0},
	{"rt_argv", 1},
	{"rt_envp", 0},
	{"rt_envc", 0},
	{"rt_errno", 0},
	{"rt_syscall", 7},
	{"rt_execve", 3},
	{"rt_dlopen", 2},
	{"rt_dlsym", 2},
	{"rt_dlclose", 1},
	{"rt_dlerror", 0},
	{"rt_globals", 0},
	{"rt_set_globals", 1},
	{"rt_get_panic_val", 0},
	{"rt_set_panic_env", 1},
	{"rt_clear_panic_env", 0},
	{"rt_jmpbuf_size", 0},
	{"rt_thread_spawn", 2},
	{"rt_thread_join", 1},
	{"rt_sleep", 1},
	{"rt_mutex_new", 0},
	{"rt_mutex_lock64", 1},
	{"rt_mutex_unlock64", 1},
	{"rt_mutex_free", 1},
	{"rt_kwarg", 2},
	{"rt_parse_ast", 1},
	{"rt_set_args", 3},
	{"rt_flt_from_int", 1},
	{"rt_flt_to_int", 1},
	{"rt_flt_trunc", 1},
	{"rt_flt_add", 2},
	{"rt_flt_sub", 2},
	{"rt_flt_mul", 2},
	{"rt_flt_div", 2},
	{"rt_flt_lt", 2},
	{"rt_flt_gt", 2},
	{"rt_flt_eq", 2},
	{"rt_flt_box_val", 1},
	{"rt_flt_unbox_val", 1},
	{"rt_rand64", 0},
	{"rt_srand", 1},
};

static bool builtin_allowed_comptime(const char *name) {
	// Disallow non-deterministic or system-interacting builtins at comptime.
	static const char *deny[] = {
		"rt_argc", "rt_argv", "rt_envp", "rt_envc", "rt_errno",
		"rt_syscall", "rt_execve",
		"rt_dlopen", "rt_dlsym", "rt_dlclose", "rt_dlerror",
		"rt_thread_spawn", "rt_thread_join", "rt_sleep",
		"rt_rand64", "rt_srand",
		"rt_globals", "rt_set_globals", "rt_set_args",
		"rt_parse_ast",
		"rt_exit",
		NULL,
	};
	for (int i = 0; deny[i]; ++i) {
		if (strcmp(name, deny[i]) == 0) return false;
	}
	return true;
}

static void add_builtins(nt_codegen *cg) {
	LLVMTypeRef fn0 = LLVMFunctionType(cg->type_i64, NULL, 0, 0);
	LLVMTypeRef fn1 = LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64}, 1, 0);
	LLVMTypeRef fn2 = LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64, cg->type_i64}, 2, 0);
	LLVMTypeRef fn3 = LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64}, 3, 0);
	LLVMTypeRef fn4 = LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64}, 4, 0);
	LLVMTypeRef fn7 = LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64}, 7, 0);
	for (size_t i = 0; i < sizeof(builtin_defs)/sizeof(builtin_defs[0]); ++i) {
		if (cg->is_comptime && !builtin_allowed_comptime(builtin_defs[i].name)) continue;
		LLVMTypeRef ty = NULL;
		switch (builtin_defs[i].args) {
			case 0: ty = fn0; break;
			case 1: ty = fn1; break;
			case 2: ty = fn2; break;
			case 3: ty = fn3; break;
			case 4: ty = fn4; break;
			case 7: ty = fn7; break;
			default: fprintf(stderr, "bad args cnt\n"); exit(1);
		}
		LLVMValueRef f = LLVMGetNamedFunction(cg->module, builtin_defs[i].name);
		if (!f) f = LLVMAddFunction(cg->module, builtin_defs[i].name, ty);
		fun_sig sig = { .name = strdup(builtin_defs[i].name), .type = ty, .value = f, .stmt = NULL, .arity = builtin_defs[i].args, .is_variadic = false };
		nt_vec_push(&cg->fun_sigs, sig);
	}
	for (int n=0; n<=13; n++) {
		char buf[32]; snprintf(buf, sizeof(buf), "rt_call%d", n);
		LLVMTypeRef *pts = alloca(sizeof(LLVMTypeRef) * (size_t)(n + 1)); for(int j=0; j<=n; j++) pts[j] = cg->type_i64;
		LLVMTypeRef cty = LLVMFunctionType(cg->type_i64, pts, (unsigned)(n + 1), 0);
		LLVMValueRef f = LLVMAddFunction(cg->module, buf, cty);
		fun_sig sig = { .name = strdup(buf), .type = cty, .value = f, .stmt = NULL, .arity = n + 1, .is_variadic = false };
		nt_vec_push(&cg->fun_sigs, sig);
	}
}

static fun_sig *lookup_fun(nt_codegen *cg, const char *name) {
	if (!cg->fun_sigs.data) return NULL;
	// 1. Try namespaced lookup if name is not qualified
	if (cg->current_mod && strchr(name, '.') == NULL) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s.%s", cg->current_mod, name);
		for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
			if (strcmp(cg->fun_sigs.data[i].name, buf) == 0) return &cg->fun_sigs.data[i];
		}
	}
	// 1b. Try common fallbacks if name is not qualified
	if (strchr(name, '.') == NULL) {
		const char *alias_full = resolve_import_alias(cg, name);
		if (alias_full) {
			return lookup_fun(cg, alias_full);
		}
		const char *fallbacks[] = {"std.core", "std.collections", "std.strings.str", "std.math", NULL};
		for (int j = 0; fallbacks[j]; ++j) {
			if (cg->current_mod && strcmp(cg->current_mod, fallbacks[j]) == 0) continue;
			char buf[256];
			snprintf(buf, sizeof(buf), "%s.%s", fallbacks[j], name);
			for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
				if (strcmp(cg->fun_sigs.data[i].name, buf) == 0) return &cg->fun_sigs.data[i];
			}
		}
	}
	// Check aliases if name has dot
	const char *dot = strchr(name, '.');
	if (dot) {
		size_t prefix_len = dot - name;
		for (size_t i = 0; i < cg->aliases.len; ++i) {
			const char *alias = cg->aliases.data[i].name;
			if (strlen(alias) == prefix_len && strncmp(name, alias, prefix_len) == 0) {
				const char *real_mod_name = (const char*)cg->aliases.data[i].stmt;
				// Avoid infinite recursion if alias matches itself
				if (strncmp(name, real_mod_name, prefix_len) == 0 && real_mod_name[prefix_len] == '\0') {
					continue;
				}
				// Construct resolved name: real_mod_name + dot + suffix
				char *resolved = malloc(strlen(real_mod_name) + strlen(dot) + 1);
				strcpy(resolved, real_mod_name);
				strcat(resolved, dot);
				fun_sig *res = lookup_fun(cg, resolved); // Recursive lookup with resolved name
				free(resolved);
				return res;
			}
		}
	}
	for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
		const char *sig_name = cg->fun_sigs.data[i].name;
		if (strcmp(sig_name, name) == 0) return &cg->fun_sigs.data[i];
		// Also try matching after the last dot if the input name is not qualified
		if (strchr(name, '.') == NULL) {
			const char *last_dot = strrchr(sig_name, '.');
			if (last_dot && strcmp(last_dot + 1, name) == 0) {
				// We found a match in a module. But is this module "used"?
				// Check if the prefix (module name) is in use_modules
				size_t mod_len = last_dot - sig_name;
				for (size_t m = 0; m < cg->use_modules.len; ++m) {
					const char *um = cg->use_modules.data[m];
					if (strlen(um) == mod_len && strncmp(um, sig_name, mod_len) == 0) {
						return &cg->fun_sigs.data[i];
					}
				}
			}
		}
	}
	return NULL;
}

static fun_sig *lookup_use_module_fun(nt_codegen *cg, const char *name, size_t argc) {
	if (!name || !*name) return NULL;
	for (size_t i = 0; i < cg->use_modules.len; ++i) {
		const char *mod = cg->use_modules.data[i];
		if (!mod) continue;
		char buf[256];
		snprintf(buf, sizeof(buf), "%s.%s", mod, name);
		fun_sig *s = resolve_overload(cg, buf, argc);
		if (s) return s;
	}
	return NULL;
}

static const char *resolve_import_alias(nt_codegen *cg, const char *name) {
	if (!cg->import_aliases.data || !name) return NULL;
	for (size_t i = 0; i < cg->import_aliases.len; ++i) {
		if (strcmp(cg->import_aliases.data[i].name, name) == 0) {
			return (const char *)cg->import_aliases.data[i].stmt;
		}
	}
	return NULL;
}

static binding *lookup_global(nt_codegen *cg, const char *name) {
	if (!cg->global_vars.data) return NULL;
	// 1. Try namespaced lookup if name is not qualified
	if (cg->current_mod && strchr(name, '.') == NULL) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s.%s", cg->current_mod, name);
		for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
			if (strcmp(cg->global_vars.data[i].name, buf) == 0) return &cg->global_vars.data[i];
		}
	}
	// 1b. Try common fallbacks if name is not qualified
	if (strchr(name, '.') == NULL) {
		const char *alias_full = resolve_import_alias(cg, name);
		if (alias_full) {
			return lookup_global(cg, alias_full);
		}
		const char *fallbacks[] = {"std.core", "std.io", "std.os", "std.core.test", NULL};
		for (int j = 0; fallbacks[j]; ++j) {
			if (cg->current_mod && strcmp(cg->current_mod, fallbacks[j]) == 0) continue;
			char buf[256];
			snprintf(buf, sizeof(buf), "%s.%s", fallbacks[j], name);
			for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
				if (strcmp(cg->global_vars.data[i].name, buf) == 0) return &cg->global_vars.data[i];
			}
		}
	}
	for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
		const char *sig_name = cg->global_vars.data[i].name;
		if (strcmp(sig_name, name) == 0) return &cg->global_vars.data[i];
		// Also try matching after the last dot if the input name is not qualified
		if (strchr(name, '.') == NULL) {
			const char *last_dot = strrchr(sig_name, '.');
			if (last_dot && strcmp(last_dot + 1, name) == 0) {
				size_t mod_len = last_dot - sig_name;
				for (size_t m = 0; m < cg->use_modules.len; ++m) {
					const char *um = cg->use_modules.data[m];
					if (strlen(um) == mod_len && strncmp(um, sig_name, mod_len) == 0) {
						return &cg->global_vars.data[i];
					}
				}
			}
		}
	}
	return NULL;
}

static fun_sig *resolve_overload(nt_codegen *cg, const char *name, size_t argc) {
	fun_sig *best = NULL; int best_score = -1;
	for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
		fun_sig *fs = &cg->fun_sigs.data[i]; if (strcmp(fs->name, name) != 0) continue;
		int score = -1;
		if (!fs->is_variadic) { if (fs->arity == (int)argc) score = 100; else if ((int)argc < fs->arity) score = 80; } else { int fixed = fs->arity - 1; if ((int)argc >= fixed) score = 60 + (int)fixed; }
		if (score > best_score) { best_score = score; best = fs; }
	}
	return best;
}

static LLVMValueRef to_bool(nt_codegen *cg, LLVMValueRef v) {
	LLVMValueRef is_none = LLVMBuildICmp(cg->builder, LLVMIntEQ, v, LLVMConstInt(cg->type_i64, 0, false), "is_none");
	LLVMValueRef is_false = LLVMBuildICmp(cg->builder, LLVMIntEQ, v, LLVMConstInt(cg->type_i64, 4, false), "is_false");
	LLVMValueRef is_zero = LLVMBuildICmp(cg->builder, LLVMIntEQ, v, LLVMConstInt(cg->type_i64, 1, false), "is_zero");
	return LLVMBuildNot(cg->builder, LLVMBuildOr(cg->builder, LLVMBuildOr(cg->builder, is_none, is_false, ""), is_zero, ""), "to_bool");
}

static binding *scope_lookup(scope *scopes, size_t depth, const char *name) {
	for (ssize_t s = (ssize_t)depth; s >= 0; --s) for (ssize_t i = (ssize_t)scopes[s].vars.len - 1; i >= 0; --i) if (strcmp(scopes[s].vars.data[i].name, name) == 0) return &scopes[s].vars.data[i];
	return NULL;
}

static void bind(scope *scopes, size_t depth, const char *name, LLVMValueRef v, nt_stmt *stmt) {
	binding b = {name, v, stmt}; nt_vec_push(&scopes[depth].vars, b);
}

static LLVMValueRef build_alloca(nt_codegen *cg, const char *name) {
	LLVMBuilderRef b = LLVMCreateBuilderInContext(cg->ctx);
	LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
	if (!f) { LLVMDisposeBuilder(b); return NULL; }
	LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(f);
	LLVMValueRef first = LLVMGetFirstInstruction(entry);
	if (first) LLVMPositionBuilderBefore(b, first); else LLVMPositionBuilderAtEnd(b, entry);
	LLVMValueRef res = LLVMBuildAlloca(b, cg->type_i64, name);
	LLVMDisposeBuilder(b); return res;
}

static void emit_defers(nt_codegen *cg, scope *scopes, size_t depth, size_t func_root) {
	for (ssize_t d = (ssize_t)depth; d >= (ssize_t)func_root; --d) for (ssize_t i = (ssize_t)scopes[d].defers.len - 1; i >= 0; --i) gen_stmt(cg, scopes, (size_t *)&d, scopes[d].defers.data[i], func_root, false);
}

static LLVMValueRef const_string_ptr(nt_codegen *cg, const char *s, size_t len) {
	for (size_t i = 0; i < cg->interns.len; ++i) if (cg->interns.data[i].len == len && memcmp(cg->interns.data[i].data, s, len) == 0) return cg->interns.data[i].val;
	const char *final_s = s; size_t final_len = len;
	size_t header_size = 64;
	size_t tail_size = 16;
	size_t total_len = header_size + final_len + 1 + tail_size;
	char *obj_data = calloc(1, total_len);
	// Write Header
	// We do NOT write heap magic numbers (NT_MAGIC1/2) here.
	// If we did, the runtime would treat this as a heap pointer and strict bounds checking
	// (rt_check_oob) would forbid accessing header fields (like length at -16).
	// By leaving magics as 0, is_heap_ptr returns false, allowing access.
	// *(uint64_t*)(obj_data) = 0x545249584E5954ULL; // NT_MAGIC1
	// *(uint64_t*)(obj_data + 8) = total_len - 128; // Raw capacity
	// *(uint64_t*)(obj_data + 16) = 0x4E59545249584EULL; // NT_MAGIC2
	*(uint64_t*)(obj_data + 48) = (final_len << 1) | 1; // Length at p-16 (tagged)
	*(uint64_t*)(obj_data + 56) = 241; // Tag at p-8 (TAG_STR)
	// Write Data
	memcpy(obj_data + header_size, final_s, final_len);
	obj_data[header_size + final_len] = '\0';
	// Write Tail
	// *(uint64_t*)(obj_data + header_size + final_len + 1) = 0xDEADBEEFCAFEBABEULL; // NT_MAGIC_END
	LLVMTypeRef arr_ty = LLVMArrayType(LLVMInt8TypeInContext(cg->ctx), (unsigned)total_len);
	LLVMValueRef g = LLVMAddGlobal(cg->module, arr_ty, ".str");
	LLVMSetInitializer(g, LLVMConstStringInContext(cg->ctx, obj_data, (unsigned)total_len, true));
	LLVMSetGlobalConstant(g, true);
	LLVMSetLinkage(g, LLVMPrivateLinkage);
	LLVMSetUnnamedAddr(g, true);
	LLVMSetAlignment(g, 64);
	// Store the global and metadata
	string_intern in = {
		.data = obj_data + header_size,
		.len = final_len,
		.val = g,
		.gv = g,
		.alloc = obj_data
	};
	nt_vec_push(&cg->interns, in);
	// Create a global i64 variable to hold the runtime pointer address
	// This is initialized to 0 but will be set in a runtime init function
	char ptr_name[128];
	snprintf(ptr_name, sizeof(ptr_name), ".str.runtime.%zu", cg->interns.len - 1);
	LLVMValueRef runtime_ptr_global = LLVMAddGlobal(cg->module, cg->type_i64, ptr_name);
	LLVMSetInitializer(runtime_ptr_global, LLVMConstInt(cg->type_i64, 0, false));
	LLVMSetLinkage(runtime_ptr_global, LLVMInternalLinkage);
	// Store this runtime pointer global in the intern struct
	cg->interns.data[cg->interns.len - 1].val = runtime_ptr_global;
	// Return the runtime pointer global (callers will load from it)
	return runtime_ptr_global;

}

static LLVMValueRef gen_binary(nt_codegen *cg, const char *op, LLVMValueRef l, LLVMValueRef r) {
	const char *rt = NULL;
	if (strcmp(op, "+") == 0) rt = "rt_add"; else if (strcmp(op, "-") == 0) rt = "rt_sub"; else if (strcmp(op, "*") == 0) rt = "rt_mul"; else if (strcmp(op, "/") == 0) rt = "rt_div"; else if (strcmp(op, "%") == 0) rt = "rt_mod";
	else if (strcmp(op, "|") == 0) rt = "rt_or"; else if (strcmp(op, "&") == 0) rt = "rt_and"; else if (strcmp(op, "^") == 0) rt = "rt_xor";
	else if (strcmp(op, "<") == 0) rt = "rt_lt"; else if (strcmp(op, "<=") == 0) rt = "rt_le"; else if (strcmp(op, ">") == 0) rt = "rt_gt"; else if (strcmp(op, ">=") == 0) rt = "rt_ge";
	else if (strcmp(op, "<<") == 0) rt = "rt_shl"; else if (strcmp(op, ">>") == 0) rt = "rt_shr";
	if (strcmp(op, "==") == 0) {
		fun_sig *s = lookup_fun(cg, "std.core.reflect.eq");
		if (!s) s = lookup_fun(cg, "eq");
		if (!s) s = lookup_fun(cg, "rt_eq");
		if (!s) {
			fprintf(stderr, "Error: '==' requires 'eq' (or rt_eq)\n");
			cg->had_error = 1;
			return LLVMConstInt(cg->type_i64, 0, false);
		}
		return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){l, r}, 2, "");
	}
	if (rt) {
		fun_sig *s = lookup_fun(cg, rt);
		if (!s) {
			fprintf(stderr, "Error: builtin %s missing\n", rt);
			cg->had_error = 1;
			return LLVMConstInt(cg->type_i64, 0, false);
		}
		return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){l, r}, 2, "");
	}
	if (strcmp(op, "!=") == 0) return LLVMBuildSub(cg->builder, LLVMConstInt(cg->type_i64, 6, false), gen_binary(cg, "==", l, r), "");
	// Simplified: handled by rt_* functions above
	if (strcmp(op, "in") == 0) {
		fun_sig *s = lookup_fun(cg, "contains");
		if (!s) {
			fprintf(stderr, "Error: 'in' requires 'contains'\n");
			cg->had_error = 1;
			return LLVMConstInt(cg->type_i64, 0, false);
		}
		return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){r, l}, 2, "");
	}
	fprintf(stderr, "Error: undef op %s\n", op);
	cg->had_error = 1;
	return LLVMConstInt(cg->type_i64, 0, false);
}

static LLVMValueRef gen_comptime_eval(nt_codegen *cg, nt_stmt *body) {
	LLVMContextRef ctx = LLVMContextCreate(); LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("ct", ctx); LLVMBuilderRef bld = LLVMCreateBuilderInContext(ctx);
	nt_codegen tcg = {.ctx = ctx, .module = mod, .builder = bld, .prog = cg->prog, .llvm_ctx_owned = true, .is_comptime = true};
	tcg.fun_sigs.len = tcg.fun_sigs.cap = 0; tcg.fun_sigs.data = NULL; tcg.interns.len = tcg.interns.cap = 0; tcg.interns.data = NULL;
	tcg.type_i64 = LLVMInt64TypeInContext(ctx); add_builtins(&tcg);
	LLVMValueRef fn = LLVMAddFunction(mod, "ctm", LLVMFunctionType(tcg.type_i64, NULL, 0, 0)); LLVMPositionBuilderAtEnd(bld, LLVMAppendBasicBlock(fn, "e"));
	scope sc[64] = {0}; size_t d = 0; gen_stmt(&tcg, sc, &d, body, 0, true);
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(bld))) LLVMBuildRet(bld, LLVMConstInt(tcg.type_i64, 1, false));
	LLVMExecutionEngineRef ee; LLVMCreateExecutionEngineForModule(&ee, mod, NULL);
	int64_t (*f)(void) = (int64_t (*)(void))LLVMGetFunctionAddress(ee, "ctm");
	int64_t res = f ? f() : 0;
	LLVMDisposeExecutionEngine(ee);
	LLVMContextDispose(ctx);
	if ((res & 1) == 0) {
		fprintf(stderr, "Error: comptime must return an int64 (tagged int)\n");
		cg->had_error = 1;
		return LLVMConstInt(cg->type_i64, 0, false);
	}
	return LLVMConstInt(cg->type_i64, res, true);
}

static LLVMValueRef gen_expr(nt_codegen *cg, scope *scopes, size_t depth, nt_expr *e) {
	// Check for dead code - don't generate instructions if block is terminated
	if (cg->builder) {
		LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
		if (cur_bb && LLVMGetBasicBlockTerminator(cur_bb)) {
			return LLVMGetUndef(cg->type_i64);
		}
	}
	if (!e || cg->had_error) return LLVMConstInt(cg->type_i64, 0, false);
	switch (e->kind) {
	case NT_E_COMPTIME: return gen_comptime_eval(cg, e->as.comptime_expr.body);
	case NT_E_LITERAL:
		if (e->as.literal.kind == NT_LIT_INT) return LLVMConstInt(cg->type_i64, (uint64_t)((e->as.literal.as.i << 1) | 1), true);
		if (e->as.literal.kind == NT_LIT_BOOL) return LLVMConstInt(cg->type_i64, e->as.literal.as.b ? 2 : 4, false);
		if (e->as.literal.kind == NT_LIT_STR) {
			// Get the runtime pointer global for this string
			LLVMValueRef str_runtime_global = const_string_ptr(cg, e->as.literal.as.s.data, e->as.literal.as.s.len);
			// Load the pointer value (will be initialized by string init function)
			return LLVMBuildLoad2(cg->builder, cg->type_i64, str_runtime_global, "str_ptr");
		}
		if (e->as.literal.kind == NT_LIT_FLOAT) {
			fun_sig *box_sig = lookup_fun(cg, "rt_flt_box_val");
			if (!box_sig) { NT_LOG_ERR("rt_flt_box_val not found\n"); cg->had_error = 1; return LLVMConstInt(cg->type_i64, 0, false); }
			LLVMValueRef fval = LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), e->as.literal.as.f);
			return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value, (LLVMValueRef[]){LLVMBuildBitCast(cg->builder, fval, cg->type_i64, "")}, 1, "");
		}
		return LLVMConstInt(cg->type_i64, 0, false);
	case NT_E_IDENT: {
		binding *b = scope_lookup(scopes, depth, e->as.ident.name); if (b) return LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
		binding *gb = lookup_global(cg, e->as.ident.name); if (gb) return LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
		fun_sig *s = lookup_fun(cg, e->as.ident.name);
		if (s) {
			LLVMValueRef sv = s->value;
			bool has_stmt = s->stmt != NULL;
			LLVMValueRef val = LLVMBuildPtrToInt(cg->builder, sv, cg->type_i64, "");
			if (has_stmt) {
				val = LLVMBuildOr(cg->builder, val, LLVMConstInt(cg->type_i64, 2, false), "");
			}
			return val;
		}
		fprintf(stderr, "Error: undef %s at %s:%d\n", e->as.ident.name, e->tok.filename ? e->tok.filename : "unknown", e->tok.line);
		// Suggest
		const char *best = NULL;
		int best_d = 100;
		// Check funs
		for(size_t i=0; i<cg->fun_sigs.len; ++i) {
			const char *cand = cg->fun_sigs.data[i].name;
			int l1 = strlen(e->as.ident.name);
			int l2 = strlen(cand);
			if (abs(l1-l2) > 3) continue;
			const char *dot = strrchr(cand, '.');
			const char *base = dot ? dot + 1 : cand;
			l2 = strlen(base);
			int d[32][32];
			if (l1 > 30) l1 = 30;
			if (l2 > 30) l2 = 30;
			for(int x=0; x<=l1; x++) d[x][0] = x;
			for(int y=0; y<=l2; y++) d[0][y] = y;
			for(int x=1; x<=l1; x++) {
				for(int y=1; y<=l2; y++) {
					int cost = (e->as.ident.name[x-1] == base[y-1]) ? 0 : 1;
					int dist_del = d[x-1][y] + 1;
					int dist_ins = d[x][y-1] + 1;
					int c_cost = d[x-1][y-1] + cost;
					int min = dist_del < dist_ins ? dist_del : dist_ins;
					if (c_cost < min) min = c_cost;
					d[x][y] = min;
				}
			}
			int dist = d[l1][l2];
			if (dist < best_d && dist < 4) { best_d = dist; best = cand; }
		}
		// Check globals
		for(size_t i=0; i<cg->global_vars.len; ++i) {
			const char *cand = cg->global_vars.data[i].name;
			int l1 = strlen(e->as.ident.name);
			int l2 = strlen(cand);
			if (abs(l1-l2) > 3) continue;
			const char *dot = strrchr(cand, '.');
			const char *base = dot ? dot + 1 : cand;
			l2 = strlen(base);
			int d[32][32];
			if (l1 > 30) l1 = 30;
			if (l2 > 30) l2 = 30;
			for(int x=0; x<=l1; x++) d[x][0] = x;
			for(int y=0; y<=l2; y++) d[0][y] = y;
			for(int x=1; x<=l1; x++) {
				for(int y=1; y<=l2; y++) {
					int cost = (e->as.ident.name[x-1] == base[y-1]) ? 0 : 1;
					int dist_del = d[x-1][y] + 1;
					int dist_ins = d[x][y-1] + 1;
					int c_cost = d[x-1][y-1] + cost;
					int min = dist_del < dist_ins ? dist_del : dist_ins;
					if (c_cost < min) min = c_cost;
					d[x][y] = min;
				}
			}
			int dist = d[l1][l2];
			if (dist < best_d && dist < 4) { best_d = dist; best = cand; }
		}
		if (best) fprintf(stderr, "       Did you mean '%s'?\n", best);
		cg->had_error = 1;
		return LLVMConstInt(cg->type_i64, 0, false);
	}
	case NT_E_UNARY: {
		LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.unary.right);
		if (strcmp(e->as.unary.op, "!") == 0) return LLVMBuildSelect(cg->builder, to_bool(cg, r), LLVMConstInt(cg->type_i64, 4, false), LLVMConstInt(cg->type_i64, 2, false), "");
		if (strcmp(e->as.unary.op, "-") == 0) {
			fun_sig *s = lookup_fun(cg, "rt_sub");
			return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false), r}, 2, "");
		}
		if (strcmp(e->as.unary.op, "~") == 0) { fun_sig *s = lookup_fun(cg, "rt_not"); return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){r}, 1, ""); }
		fprintf(stderr, "Error: unsupported unary op %s\n", e->as.unary.op);
		cg->had_error = 1;
		return LLVMConstInt(cg->type_i64, 0, false);
	}
	case NT_E_BINARY: return gen_binary(cg, e->as.binary.op, gen_expr(cg, scopes, depth, e->as.binary.left), gen_expr(cg, scopes, depth, e->as.binary.right));
	case NT_E_CALL:
	case NT_E_MEMCALL: {
		nt_expr_call *c = (e->kind == NT_E_CALL) ? &e->as.call : NULL;
		nt_expr_memcall *mc = (e->kind == NT_E_MEMCALL) ? &e->as.memcall : NULL;
		LLVMValueRef callee = NULL;
		LLVMTypeRef ft = NULL;
		LLVMValueRef fv = NULL;
		bool is_variadic = false;
		int sig_arity = 0;
		bool has_sig = false;
		bool skip_target = false;
		if (mc) {
			char buf[128];
			const char *prefixes[] = {"dict_", "list_", "str_", "set_", "bytes_", "queue_", "heap_", "bigint_", NULL};
			fun_sig *sig_found = NULL;
			// Priority 1: Check if target is a module alias
			if (mc->target->kind == NT_E_IDENT) {
				const char *target_name = mc->target->as.ident.name;
				const char *module_name = target_name;
				for (size_t k = 0; k < cg->aliases.len; ++k) {
					if (strcmp(cg->aliases.data[k].name, target_name) == 0) {
						module_name = (const char*)cg->aliases.data[k].stmt;
						break;
					}
				}
				// If it's an alias, or if it's NOT a local function/variable/keyword,
				// it might be a module call (e.g. m.add)
				if (module_name != target_name || (lookup_fun(cg, target_name) == NULL && scope_lookup(scopes, depth, target_name) == NULL)) {
					char dotted[256];
					snprintf(dotted, sizeof(dotted), "%s.%s", module_name, mc->name);
					sig_found = lookup_fun(cg, dotted);
					if (sig_found) {
						ft = sig_found->type;
						fv = sig_found->value;
						sig_arity = sig_found->arity;
						is_variadic = sig_found->is_variadic;
						has_sig = true;
						skip_target = true;
						callee = fv;
						goto static_call_handling;
					}
				}
			}
			// Priority 2: Check standard prefixes (dict_, list_, etc.)
			// Priority 1: Check if target is a module alias
			if (mc->target->kind == NT_E_IDENT) {
				const char *target_name = mc->target->as.ident.name;
				const char *module_name = target_name;
				bool is_alias = false;
				for (size_t k = 0; k < cg->aliases.len; ++k) {
					if (strcmp(cg->aliases.data[k].name, target_name) == 0) {
						module_name = (const char*)cg->aliases.data[k].stmt;
						is_alias = true;
						break;
					}
				}
				// If it's an alias, it MUST be a module call.
				// If it's NOT an alias, check if it doesn't exist as a local variable/function,
				// in which case it might be a direct module usage (e.g. math.add)
				if (is_alias || (lookup_fun(cg, target_name) == NULL && scope_lookup(scopes, depth, target_name) == NULL)) {
					char dotted[256];
					snprintf(dotted, sizeof(dotted), "%s.%s", module_name, mc->name);
					sig_found = lookup_fun(cg, dotted);
					if (sig_found) {
						ft = sig_found->type;
						fv = sig_found->value;
						sig_arity = sig_found->arity;
						is_variadic = sig_found->is_variadic;
						has_sig = true;
						callee = fv;
						goto static_call_handling;
					}
					// If it was an ALIAS, but method not found, we shouldn't fall back to standard methods
					if (is_alias) {
						fprintf(stderr, "Error: function %s.%s not found\n", module_name, mc->name);
						cg->had_error = 1;
						return LLVMConstInt(cg->type_i64, 0, false);
					}
				}
			}
			// Priority 2: Check standard prefixes (dict_, list_, etc.)
			for (int i=0; prefixes[i]; i++) {
				snprintf(buf, sizeof(buf), "%s%s", prefixes[i], mc->name);
				sig_found = lookup_fun(cg, buf);
				if (sig_found) break;
			}
			// Priority 3: Direct name
			if (!sig_found) sig_found = lookup_fun(cg, mc->name);
			static_call_handling: ;
			if (!sig_found) {
				const char *tname = (mc && mc->target->kind == NT_E_IDENT) ? mc->target->as.ident.name : "<expr>";
				fprintf(stderr, "Error: function %s.%s not found\n", tname, mc->name);
				// Suggest corrections
				const char *best_match = NULL;
				int best_dist = 100;
				for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
					const char *candidate = cg->fun_sigs.data[i].name;
					// Simple distance check: Levenshtein or substring
					// We'll implemented a simple distance inline to avoid large function
					int len1 = strlen(mc->name);
					int len2 = strlen(candidate);
					if (abs(len1 - len2) > 3) continue;
					// Substring match for namespacing suggestions?
					// Or just check suffix matching?
					const char *dot = strrchr(candidate, '.');
					const char *base = dot ? dot + 1 : candidate;
					// Levenshtein on base name
					int d[32][32]; // Max name length 31 for suggestion optimization
					int l1 = strlen(mc->name);
					int l2 = strlen(base);
					if (l1 > 30) l1 = 30;
					if (l2 > 30) l2 = 30;
					for(int x=0; x<=l1; x++) d[x][0] = x;
					for(int y=0; y<=l2; y++) d[0][y] = y;
					for(int x=1; x<=l1; x++) {
						for(int y=1; y<=l2; y++) {
							int cost = (mc->name[x-1] == base[y-1]) ? 0 : 1;
							int a = d[x-1][y] + 1;
							int b = d[x][y-1] + 1;
							int cost_sub = d[x-1][y-1] + cost;
							int min = a < b ? a : b;
							if (cost_sub < min) min = cost_sub;
							d[x][y] = min;
						}
					}
					int dist = d[l1][l2];
					if (dist < best_dist && dist < 4) {
						best_dist = dist;
						best_match = candidate;
					}
				}
				if (best_match) {
					fprintf(stderr, "       Did you mean '%s'?\n", best_match);
				}
				cg->had_error = 1;
				return LLVMConstInt(cg->type_i64, 0, false);
			}
			ft = sig_found->type;
			fv = sig_found->value;
			sig_arity = sig_found->arity;
			is_variadic = sig_found->is_variadic;
			has_sig = true;
			callee = fv;
		} else {
			const char *name = (c->callee->kind == NT_E_IDENT) ? c->callee->as.ident.name : NULL;
			if (name) {
				binding *b = scope_lookup(scopes, depth, name);
				if (b) {
					callee = LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
				} else {
					binding *gb = lookup_global(cg, name);
					if (gb) callee = LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
				}
			}
			if (!callee) {
				fun_sig *sig_found = name ? resolve_overload(cg, name, c->args.len) : NULL;
				if (!sig_found && name) sig_found = lookup_use_module_fun(cg, name, c->args.len);
				if (sig_found) {
					ft = sig_found->type;
					fv = sig_found->value;
					sig_arity = sig_found->arity;
					is_variadic = sig_found->is_variadic;
					has_sig = true;
					callee = fv;
				} else {
					callee = gen_expr(cg, scopes, depth, c->callee);
				}
			}
		}
		if (!ft) {
			size_t n = c ? c->args.len : (mc->args.len + 1);
			char buf[32]; snprintf(buf, sizeof(buf), "rt_call%zu", n);
			fun_sig *rsig = lookup_fun(cg, buf);
			if (!rsig) {
				fprintf(stderr, "%serror (linker): undefined symbol '%s'%s\n",
					nt_clr(NT_CLR_RED), buf, nt_clr(NT_CLR_RESET));
				const char *best_match = NULL;
				for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
					const char *candidate = cg->fun_sigs.data[i].name;
					if (strstr(candidate, buf) || strstr(buf, candidate)) {
						best_match = candidate;
						break;
					}
				}
				if (best_match) {
					fprintf(stderr, "  %snote:%s did you mean '%s'?\n",
						nt_clr(NT_CLR_YELLOW), nt_clr(NT_CLR_RESET), best_match);
				}
				cg->had_error = 1;
				return LLVMConstInt(cg->type_i64, 0, false);
			}
			LLVMTypeRef rty = rsig->type;
			LLVMValueRef rval = rsig->value;
			LLVMValueRef callee_int = (LLVMTypeOf(callee) == cg->type_i64) ? callee : LLVMBuildPtrToInt(cg->builder, callee, cg->type_i64, "callee_int");
			LLVMValueRef *call_args = malloc(sizeof(LLVMValueRef) * (n + 1));
			call_args[0] = callee_int;
			if (c) {
				for (size_t i=0; i < n; i++) call_args[i+1] = gen_expr(cg, scopes, depth, c->args.data[i].val);
			} else {
				call_args[1] = gen_expr(cg, scopes, depth, mc->target);
				for (size_t i=0; i < mc->args.len; i++) call_args[i+2] = gen_expr(cg, scopes, depth, mc->args.data[i].val);
			}
			LLVMValueRef res = LLVMBuildCall2(cg->builder, rty, rval, call_args, (unsigned)n + 1, "");
			free(call_args);
			return res;
		}
		size_t call_argc = c ? c->args.len : (skip_target ? mc->args.len : mc->args.len + 1);
		size_t sig_argc = (has_sig && is_variadic) ? (size_t)sig_arity : (has_sig ? (size_t)sig_arity : call_argc);
		size_t final_argc = (sig_argc > call_argc) ? sig_argc : call_argc;
		LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * final_argc);
		size_t user_args_len = c ? c->args.len : mc->args.len;
		nt_call_arg *user_args = c ? c->args.data : mc->args.data;
		for (size_t i=0; i < final_argc; i++) {
			size_t user_idx = (mc && !skip_target) ? (i - 1) : i;
			if (mc && !skip_target && i == 0) {
				args[i] = gen_expr(cg, scopes, depth, mc->target);
			} else if (has_sig && is_variadic && i == (size_t)sig_arity - 1) {
				/* Variadic packaging */
				fun_sig *ls_s = lookup_fun(cg, "list");
				if(!ls_s) ls_s = lookup_fun(cg, "std.core.list");
				fun_sig *as_s = lookup_fun(cg, "append");
				if(!as_s) as_s = lookup_fun(cg, "std.core.append");
				if (!ls_s || !as_s) {
					fprintf(stderr, "Error: variadic arguments require 'list' and 'append' functions to be defined\n");
					cg->had_error = 1;
					return LLVMConstInt(cg->type_i64, 0, false);
				}
				LLVMTypeRef lty = ls_s->type, aty = as_s->type;
				LLVMValueRef lval = ls_s->value, aval = as_s->value;
				LLVMValueRef vl = LLVMBuildCall2(cg->builder, lty, lval, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 35, false)}, 1, "");
				for (size_t j = user_idx; j < user_args_len; j++) {
					nt_call_arg *a = &user_args[j];
					LLVMValueRef av = gen_expr(cg, scopes, depth, a->val);
					if (a->name) {
						fun_sig *ks_s = lookup_fun(cg, "rt_kwarg");
						if (!ks_s) {
							fprintf(stderr, "Error: keyword args require 'rt_kwarg'\n");
							cg->had_error = 1;
							return LLVMConstInt(cg->type_i64, 0, false);
						}
						LLVMTypeRef kty = ks_s->type; LLVMValueRef kval = ks_s->value;
					LLVMValueRef name_runtime_global = const_string_ptr(cg, a->name, strlen(a->name));
					LLVMValueRef name_ptr = LLVMBuildLoad2(cg->builder, cg->type_i64, name_runtime_global, "");
					av = LLVMBuildCall2(cg->builder, kty, kval, (LLVMValueRef[]){name_ptr, av}, 2, "");
					}
					vl = LLVMBuildCall2(cg->builder, aty, aval, (LLVMValueRef[]){vl, av}, 2, "");
				}
				args[i] = vl; break;
			} else if (user_idx < user_args_len) {
				args[i] = gen_expr(cg, scopes, depth, user_args[user_idx].val);
			} else if (has_sig && sig_arity > (int)i && i < user_args_len) { // fallback
				 args[i] = LLVMConstInt(cg->type_i64, 0, false);
			} else {
				args[i] = LLVMConstInt(cg->type_i64, 0, false);
			}
		}
		if (has_sig) {
			/* const char *callee_name = (c && c->callee->kind == NT_E_IDENT) ? c->callee->as.ident.name : (mc ? mc->name : "ptr"); */
			/* fprintf(stderr, "DEBUG: Call gen '%s' - is_variadic: %d, sig_arity: %d, call_argc: %zu\n", callee_name, is_variadic, sig_arity, c ? c->args.len : mc->args.len); */
		}
		LLVMValueRef res = LLVMBuildCall2(cg->builder, ft, callee, args, (unsigned)(has_sig && is_variadic ? (size_t)sig_arity : final_argc), "");
		free(args);
		return res;
	}
	case NT_E_INDEX: {
		if (e->as.index.stop || e->as.index.step || !e->as.index.start) {
			fun_sig *s = lookup_fun(cg, "slice");
			if (!s) {
				fprintf(stderr, "Error: slice requires 'slice'\n");
				cg->had_error = 1;
				return LLVMConstInt(cg->type_i64, 0, false);
			}
			LLVMValueRef start = e->as.index.start ? gen_expr(cg, scopes, depth, e->as.index.start) : LLVMConstInt(cg->type_i64, 1, false); // 0 tagged
			LLVMValueRef stop = e->as.index.stop ? gen_expr(cg, scopes, depth, e->as.index.stop) : LLVMConstInt(cg->type_i64, ((0x3fffffffULL) << 1) | 1, false);
			LLVMValueRef step = e->as.index.step ? gen_expr(cg, scopes, depth, e->as.index.step) : LLVMConstInt(cg->type_i64, 3, false); // 1 tagged
			return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target), start, stop, step}, 4, "");
		}
		fun_sig *s = lookup_fun(cg, "get");
		if (!s) s = lookup_fun(cg, "std.core.get");
		if (!s) {
			fprintf(stderr, "Error: index requires 'get'\n");
			cg->had_error = 1;
			return LLVMConstInt(cg->type_i64, 0, false);
		}
		return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target), gen_expr(cg, scopes, depth, e->as.index.start)}, 2, "");
	}
	case NT_E_LIST:
	case NT_E_TUPLE: {
		fun_sig *ls = lookup_fun(cg, "list");
		if (!ls) ls = lookup_fun(cg, "std.core.list");
		fun_sig *as = lookup_fun(cg, "append");
		if (!as) as = lookup_fun(cg, "std.core.append");
		if (!ls || !as) {
			fprintf(stderr, "Error: list requires list/append (searched 'list', 'std.core.list', 'append', 'std.core.append')\n");
			cg->had_error = 1;
			return LLVMConstInt(cg->type_i64, 0, false);
		}
		LLVMValueRef vl = LLVMBuildCall2(cg->builder, ls->type, ls->value, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)((e->as.list_like.len << 1) | 1), false)}, 1, "");
		for (size_t i=0; i<e->as.list_like.len; i++) vl = LLVMBuildCall2(cg->builder, as->type, as->value, (LLVMValueRef[]){vl, gen_expr(cg, scopes, depth, e->as.list_like.data[i])}, 2, "");
		return vl;
	}
	case NT_E_DICT: {
		fun_sig *ds = lookup_fun(cg, "dict");
		if (!ds) ds = lookup_fun(cg, "std.collections.dict.dict");
		fun_sig *ss = lookup_fun(cg, "setitem");
		if (!ss) ss = lookup_fun(cg, "std.collections.dict.setitem");
		if (!ds || !ss) {
			fprintf(stderr, "Error: dict requires dict/setitem (searched 'dict', 'std.collections.dict.dict', 'setitem', 'std.collections.dict.setitem')\n");
			cg->had_error = 1;
			return LLVMConstInt(cg->type_i64, 0, false);
		}
		LLVMValueRef dl = LLVMBuildCall2(cg->builder, ds->type, ds->value, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)((e->as.dict.pairs.len << 2) | 1), false)}, 1, "");
		for (size_t i=0; i<e->as.dict.pairs.len; i++) LLVMBuildCall2(cg->builder, ss->type, ss->value, (LLVMValueRef[]){dl, gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].key), gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].value)}, 3, "");
		return dl;
	}
	case NT_E_LOGICAL: {
		bool and = strcmp(e->as.logical.op, "&&") == 0; LLVMValueRef left = to_bool(cg, gen_expr(cg, scopes, depth, e->as.logical.left));
		LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder); LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
		LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlock(f, "lrhs"), end_bb = LLVMAppendBasicBlock(f, "lend");
		if (and) LLVMBuildCondBr(cg->builder, left, rhs_bb, end_bb); else LLVMBuildCondBr(cg->builder, left, end_bb, rhs_bb);
		LLVMPositionBuilderAtEnd(cg->builder, rhs_bb); LLVMValueRef rv = gen_expr(cg, scopes, depth, e->as.logical.right); LLVMBuildBr(cg->builder, end_bb);
		LLVMBasicBlockRef rend_bb = LLVMGetInsertBlock(cg->builder); LLVMPositionBuilderAtEnd(cg->builder, end_bb);
		LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "");
		LLVMAddIncoming(phi, (LLVMValueRef[]){and ? LLVMConstInt(cg->type_i64, 4, false) : LLVMConstInt(cg->type_i64, 2, false), rv}, (LLVMBasicBlockRef[]){cur_bb, rend_bb}, 2); return phi;
	}
	case NT_E_TERNARY: {
		LLVMValueRef cond = to_bool(cg, gen_expr(cg, scopes, depth, e->as.ternary.cond));
		LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
		LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
		LLVMBasicBlockRef true_bb = LLVMAppendBasicBlock(f, "tern_true");
		LLVMBasicBlockRef false_bb = LLVMAppendBasicBlock(f, "tern_false");
		LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(f, "tern_end");
		LLVMBuildCondBr(cg->builder, cond, true_bb, false_bb);
		LLVMPositionBuilderAtEnd(cg->builder, true_bb);
		LLVMValueRef true_val = gen_expr(cg, scopes, depth, e->as.ternary.true_expr);
		LLVMBuildBr(cg->builder, end_bb);
		LLVMBasicBlockRef true_end_bb = LLVMGetInsertBlock(cg->builder);
		LLVMPositionBuilderAtEnd(cg->builder, false_bb);
		LLVMValueRef false_val = gen_expr(cg, scopes, depth, e->as.ternary.false_expr);
		LLVMBuildBr(cg->builder, end_bb);
		LLVMBasicBlockRef false_end_bb = LLVMGetInsertBlock(cg->builder);
		LLVMPositionBuilderAtEnd(cg->builder, end_bb);
		LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "tern");
		LLVMAddIncoming(phi, (LLVMValueRef[]){true_val, false_val}, (LLVMBasicBlockRef[]){true_end_bb, false_end_bb}, 2);
		return phi;
	}
	case NT_E_ASM: {
		unsigned nargs = e->as.as_asm.args.len;
		LLVMValueRef llvm_args[nargs > 0 ? nargs : 1];
		LLVMTypeRef arg_types[nargs > 0 ? nargs : 1];
		for (unsigned i = 0; i < nargs; ++i) {
			 llvm_args[i] = gen_expr(cg, scopes, depth, e->as.as_asm.args.data[i]);
			 arg_types[i] = cg->type_i64;
		}
		LLVMTypeRef func_type = LLVMFunctionType(cg->type_i64, arg_types, nargs, false);
		LLVMValueRef asm_val = LLVMConstInlineAsm(func_type, e->as.as_asm.code, e->as.as_asm.constraints, true, false);
		return LLVMBuildCall2(cg->builder, func_type, asm_val, llvm_args, nargs, "");
	}
	case NT_E_FSTRING: {
		// Empty string init
		LLVMValueRef empty_runtime_global = const_string_ptr(cg, "", 0);
		LLVMValueRef res = LLVMBuildLoad2(cg->builder, cg->type_i64, empty_runtime_global, "");
		fun_sig *cs = lookup_fun(cg, "rt_str_concat"), *ts = lookup_fun(cg, "rt_to_str");
		for (size_t i=0; i<e->as.fstring.parts.len; i++) {
			nt_fstring_part p = e->as.fstring.parts.data[i];
			LLVMValueRef pv;
			if (p.kind == NT_FSP_STR) {
				LLVMValueRef part_runtime_global = const_string_ptr(cg, p.as.s.data, p.as.s.len);
				pv = LLVMBuildLoad2(cg->builder, cg->type_i64, part_runtime_global, "");
			} else {
				pv = LLVMBuildCall2(cg->builder, ts->type, ts->value, (LLVMValueRef[]){gen_expr(cg, scopes, depth, p.as.e)}, 1, "");
			}
			res = LLVMBuildCall2(cg->builder, cs->type, cs->value, (LLVMValueRef[]){res, pv}, 2, "");
		}
		return res;
	}
	case NT_E_LAMBDA:
	case NT_E_FN: {
		/* Capture All Visible Variables (scopes[1..depth]) */
		binding_list captures = {0};
		for (ssize_t i=1; i<=(ssize_t)depth; i++) {
			for (size_t j=0; j<scopes[i].vars.len; j++) {
				nt_vec_push(&captures, scopes[i].vars.data[j]);
			}
		}
		char name[64]; snprintf(name, sizeof(name), "__lambda_%d", cg->lambda_count++);
		nt_stmt sfn = { .kind = NT_S_FUNC, .as.fn = { .name = strdup(name), .params = e->as.lambda.params, .body = e->as.lambda.body, .is_variadic = e->as.lambda.is_variadic } };
		scope sc[64] = {0};
		gen_func(cg, &sfn, name, sc, 0, &captures);
		free((void*)sfn.as.fn.name);
		LLVMValueRef lf = LLVMGetNamedFunction(cg->module, name);
		LLVMValueRef fn_ptr_tagged = LLVMBuildOr(cg->builder, LLVMBuildPtrToInt(cg->builder, lf, cg->type_i64, ""), LLVMConstInt(cg->type_i64, 2, false), "");
		if (captures.len == 0 && e->kind != NT_E_LAMBDA) {
			 /* Standard function (tag 2) if no captures */
			 nt_vec_free(&captures);
			 return fn_ptr_tagged;
		}
		/* Create Env */
		fun_sig *malloc_sig = lookup_fun(cg, "rt_malloc");
		if (!malloc_sig) { fprintf(stderr, "Error: rt_malloc required for closures\n"); cg->had_error = 1; return LLVMConstInt(cg->type_i64, 0, false); }
		LLVMValueRef env_alloc_size = LLVMConstInt(cg->type_i64, (uint64_t)((captures.len * 8) << 1) | 1, false);
		LLVMValueRef env_ptr = LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value, (LLVMValueRef[]){env_alloc_size}, 1, "env");
		LLVMValueRef env_raw = LLVMBuildIntToPtr(cg->builder, env_ptr, LLVMPointerType(cg->type_i64, 0), "env_raw");
		for (size_t i=0; i<captures.len; i++) {
			LLVMValueRef slot_val = LLVMBuildLoad2(cg->builder, cg->type_i64, captures.data[i].value, "");
			LLVMValueRef dst = LLVMBuildGEP2(cg->builder, cg->type_i64, env_raw, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1, "");
			LLVMBuildStore(cg->builder, slot_val, dst);
		}
		/* Create Closure Object [Tag=105 | Code | Env] */
		LLVMValueRef cls_size = LLVMConstInt(cg->type_i64, (16 << 1) | 1, false);
		LLVMValueRef cls_ptr = LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value, (LLVMValueRef[]){cls_size}, 1, "closure");
		LLVMValueRef cls_raw = LLVMBuildIntToPtr(cg->builder, cls_ptr, LLVMPointerType(cg->type_i64, 0), "");
		/* Set Tag -8 */
		LLVMValueRef tag_addr = LLVMBuildGEP2(cg->builder, LLVMInt8TypeInContext(cg->ctx), LLVMBuildBitCast(cg->builder, cls_raw, LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), ""), (LLVMValueRef[]){LLVMConstInt(cg->type_i64, -8, true)}, 1, "");
		LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 105, false), LLVMBuildBitCast(cg->builder, tag_addr, LLVMPointerType(cg->type_i64, 0), ""));
		/* Store Code at 0 */
		LLVMBuildStore(cg->builder, fn_ptr_tagged, cls_raw);
		/* Store Env at 8 */
		LLVMValueRef env_store_addr = LLVMBuildGEP2(cg->builder, cg->type_i64, cls_raw, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false)}, 1, "");
		LLVMBuildStore(cg->builder, env_ptr, env_store_addr);
		nt_vec_free(&captures);
		return cls_ptr;
	}
	case NT_E_MATCH: {
		LLVMValueRef old_store = cg->result_store;
		LLVMValueRef slot = build_alloca(cg, "match_res");
		LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), slot);
		cg->result_store = slot;
		nt_stmt fake = { .kind = NT_S_MATCH, .as.match = e->as.match, .tok = e->tok };
		size_t d = depth;
		gen_stmt(cg, scopes, &d, &fake, cg->func_root, true);
		cg->result_store = old_store;
		return LLVMBuildLoad2(cg->builder, cg->type_i64, slot, "");
	}
	default: {
		const char *fname = e->tok.filename ? e->tok.filename : "<input>";
		fprintf(stderr, "Error: unsupported expr kind %d token_kind=%d at %s:%d token='%.*s'\n",
				e->kind, e->tok.kind, fname, e->tok.line,
				(int)e->tok.len, e->tok.lexeme ? e->tok.lexeme : "");
		cg->had_error = 1;
		return LLVMConstInt(cg->type_i64, 0, false);
	}
	}
}

static void gen_stmt(nt_codegen *cg, scope *scopes, size_t *depth, nt_stmt *s, size_t func_root, bool is_tail) {
	if (!s || cg->had_error) return;
	switch (s->kind) {
	case NT_S_VAR: {
		for (size_t i=0; i<s->as.var.names.len; i++) {
			const char *n = s->as.var.names.data[i]; LLVMValueRef slot;
			if (s->as.var.is_undef) {
				if (*depth == 0) {
					binding *gb = lookup_global(cg, n);
					if (!gb) {
						fprintf(stderr, "Error: undef %s at %s:%d\n", n, s->tok.filename ? s->tok.filename : "unknown", s->tok.line);
						cg->had_error = 1;
						return;
					}
					slot = gb->value;
				} else {
					binding *eb = scope_lookup(scopes, *depth, n);
					if (!eb) eb = lookup_global(cg, n);
					if (!eb) {
						fprintf(stderr, "Error: undef %s at %s:%d\n", n, s->tok.filename ? s->tok.filename : "unknown", s->tok.line);
						cg->had_error = 1;
						return;
					}
					slot = eb->value;
				}
				LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 0, false), slot);
				continue;
			}
			if (*depth == 0) {
				binding *gb = lookup_global(cg, n);
				if (gb) slot = gb->value;
				else {
					slot = LLVMAddGlobal(cg->module, cg->type_i64, n);
					LLVMSetInitializer(slot, LLVMConstInt(cg->type_i64, 0, false));
					binding b = { strdup(n), slot, NULL };
					nt_vec_push(&cg->global_vars, b);
				}
			} else {
				if (s->as.var.is_decl) {
					slot = build_alloca(cg, n);
					bind(scopes, *depth, n, slot, NULL);
				} else {
					binding *eb = scope_lookup(scopes, *depth, n);
					if (eb) {
						slot = eb->value;
					} else {
						// Check global scope as well
						binding *gb = lookup_global(cg, n);
						if (gb) {
							slot = gb->value;
						} else {
							slot = build_alloca(cg, n);
							bind(scopes, *depth, n, slot, NULL);
						}
					}
				}
			}
			LLVMValueRef val = gen_expr(cg, scopes, *depth, s->as.var.expr);
			if (!cg->builder) { fprintf(stderr, "ERROR: NULL builder in NT_S_VAR\n"); exit(1); }
			if (!val) { fprintf(stderr, "ERROR: NULL val in NT_S_VAR\n"); exit(1); }
			if (!slot) { fprintf(stderr, "ERROR: NULL slot in NT_S_VAR\n"); exit(1); }
			LLVMBasicBlockRef cur_block = LLVMGetInsertBlock(cg->builder);
			if (!cur_block) { fprintf(stderr, "ERROR: NULL block in NT_S_VAR\n"); exit(1); }
			if (!LLVMGetBasicBlockTerminator(cur_block)) {
				LLVMBuildStore(cg->builder, val, slot);
			}
		}
		break;
	}
	case NT_S_EXPR: {
		LLVMValueRef v = gen_expr(cg, scopes, *depth, s->as.expr.expr);
		if (is_tail) {
			if (cg->result_store) {
				if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
					LLVMBuildStore(cg->builder, v, cg->result_store);
				}
			} else {
				emit_defers(cg, scopes, *depth, func_root);
				if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildRet(cg->builder, v);
			}
		}
		break;
	}
	case NT_S_IF: {
		LLVMValueRef c = to_bool(cg, gen_expr(cg, scopes, *depth, s->as.iff.test)); LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
		LLVMBasicBlockRef tb = LLVMAppendBasicBlock(f, "it"), eb = s->as.iff.alt ? LLVMAppendBasicBlock(f, "ie") : NULL, next = LLVMAppendBasicBlock(f, "in");
		LLVMBuildCondBr(cg->builder, c, tb, eb ? eb : next);
		LLVMPositionBuilderAtEnd(cg->builder, tb); gen_stmt(cg, scopes, depth, s->as.iff.conseq, func_root, is_tail); if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, next);
		if (eb) { LLVMPositionBuilderAtEnd(cg->builder, eb); gen_stmt(cg, scopes, depth, s->as.iff.alt, func_root, is_tail); if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, next); }
		LLVMPositionBuilderAtEnd(cg->builder, next); break;
	}
	case NT_S_MATCH: {
		LLVMValueRef testv = gen_expr(cg, scopes, *depth, s->as.match.test);
		LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
		LLVMBasicBlockRef end = LLVMAppendBasicBlock(f, "match_end");
		LLVMTypeRef i1 = LLVMInt1TypeInContext(cg->ctx);
		for (size_t i = 0; i < s->as.match.arms.len; ++i) {
			nt_match_arm *arm = &s->as.match.arms.data[i];
			LLVMBasicBlockRef arm_bb = LLVMAppendBasicBlock(f, "match_arm");
			LLVMBasicBlockRef next_bb = LLVMAppendBasicBlock(f, "match_next");
			LLVMValueRef cond = NULL;
			int has_wild = 0;
			for (size_t j = 0; j < arm->patterns.len; ++j) {
				nt_expr *pat = arm->patterns.data[j];
				if (pat && pat->kind == NT_E_IDENT && pat->as.ident.name && strcmp(pat->as.ident.name, "_") == 0) {
					has_wild = 1;
					break;
				}
				LLVMValueRef pv = gen_expr(cg, scopes, *depth, pat);
				LLVMValueRef eq = gen_binary(cg, "==", testv, pv);
				LLVMValueRef c = to_bool(cg, eq);
				cond = cond ? LLVMBuildOr(cg->builder, cond, c, "") : c;
			}
			if (has_wild) {
				cond = LLVMConstInt(i1, 1, false);
			} else if (!cond) {
				cond = LLVMConstInt(i1, 0, false);
			}
			LLVMBuildCondBr(cg->builder, cond, arm_bb, next_bb);
			LLVMPositionBuilderAtEnd(cg->builder, arm_bb);
			gen_stmt(cg, scopes, depth, arm->conseq, func_root, is_tail);
			if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, end);
			LLVMPositionBuilderAtEnd(cg->builder, next_bb);
		}
		if (s->as.match.default_conseq) {
			gen_stmt(cg, scopes, depth, s->as.match.default_conseq, func_root, is_tail);
			if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, end);
		} else {
			LLVMBuildBr(cg->builder, end);
		}
		LLVMPositionBuilderAtEnd(cg->builder, end);
		break;
	}
	case NT_S_WHILE: {
		LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
		LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "wc"), bb = LLVMAppendBasicBlock(f, "wb"), eb = LLVMAppendBasicBlock(f, "we");
		LLVMBuildBr(cg->builder, cb); LLVMPositionBuilderAtEnd(cg->builder, cb);
		LLVMBuildCondBr(cg->builder, to_bool(cg, gen_expr(cg, scopes, *depth, s->as.whl.test)), bb, eb);
		LLVMPositionBuilderAtEnd(cg->builder, bb); (*depth)++; scopes[*depth].vars.len = scopes[*depth].vars.cap = 0; scopes[*depth].vars.data = NULL; scopes[*depth].defers.len = scopes[*depth].defers.cap = 0; scopes[*depth].defers.data = NULL; scopes[*depth].break_bb = eb; scopes[*depth].continue_bb = cb;
		gen_stmt(cg, scopes, depth, s->as.whl.body, func_root, false);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
			emit_defers(cg, scopes, *depth, func_root);
			if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, cb);
		}
		nt_vec_free(&scopes[*depth].defers); nt_vec_free(&scopes[*depth].vars); (*depth)--;
		LLVMPositionBuilderAtEnd(cg->builder, eb); break;
	}
	case NT_S_FOR: {
		LLVMValueRef itv = gen_expr(cg, scopes, *depth, s->as.fr.iterable);
		LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
		LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "fc"), bb = LLVMAppendBasicBlock(f, "fb"), eb = LLVMAppendBasicBlock(f, "fe");
		LLVMValueRef idx_p = build_alloca(cg, "idx"); LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), idx_p);
		fun_sig *ls = lookup_fun(cg, "list_len"), *gs = lookup_fun(cg, "get");
		if (!ls || !gs) {
			fprintf(stderr, "Error: for requires list_len/get\n");
			cg->had_error = 1;
			return;
		}
		LLVMBuildBr(cg->builder, cb); LLVMPositionBuilderAtEnd(cg->builder, cb);
		LLVMValueRef i_val = LLVMBuildLoad2(cg->builder, cg->type_i64, idx_p, "");
		LLVMValueRef n_val = LLVMBuildCall2(cg->builder, ls->type, ls->value, (LLVMValueRef[]){itv}, 1, "");
		LLVMBuildCondBr(cg->builder, LLVMBuildICmp(cg->builder, LLVMIntSLT, i_val, n_val, ""), bb, eb);
		LLVMPositionBuilderAtEnd(cg->builder, bb);
		LLVMValueRef item = LLVMBuildCall2(cg->builder, gs->type, gs->value, (LLVMValueRef[]){itv, i_val}, 2, "");
		LLVMValueRef iv = build_alloca(cg, s->as.fr.iter_var); LLVMBuildStore(cg->builder, item, iv);
		LLVMBuildStore(cg->builder, LLVMBuildAdd(cg->builder, i_val, LLVMConstInt(cg->type_i64, 2, false), ""), idx_p);
		(*depth)++; scopes[*depth].vars.len = scopes[*depth].vars.cap = 0; scopes[*depth].vars.data = NULL; scopes[*depth].defers.len = scopes[*depth].defers.cap = 0; scopes[*depth].defers.data = NULL; scopes[*depth].break_bb = eb; scopes[*depth].continue_bb = cb; bind(scopes, *depth, s->as.fr.iter_var, iv, NULL);
		gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
			emit_defers(cg, scopes, *depth, func_root);
			if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, cb);
		}
		nt_vec_free(&scopes[*depth].defers); nt_vec_free(&scopes[*depth].vars); (*depth)--;
		LLVMPositionBuilderAtEnd(cg->builder, eb); break;
	}
	case NT_S_RETURN: {
		emit_defers(cg, scopes, *depth, func_root); if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
		LLVMBuildRet(cg->builder, s->as.ret.value ? gen_expr(cg, scopes, *depth, s->as.ret.value) : LLVMConstInt(cg->type_i64, 1, false)); break;
	}
	case NT_S_USE: {
		break;
	}
	case NT_S_BLOCK: {
		(*depth)++; scopes[*depth].vars.len = scopes[*depth].vars.cap = 0; scopes[*depth].vars.data = NULL; scopes[*depth].defers.len = scopes[*depth].defers.cap = 0; scopes[*depth].defers.data = NULL; scopes[*depth].break_bb = scopes[*depth-1].break_bb; scopes[*depth].continue_bb = scopes[*depth-1].continue_bb;
		for (size_t i=0; i<s->as.block.body.len; i++) gen_stmt(cg, scopes, depth, s->as.block.body.data[i], func_root, is_tail && (i == s->as.block.body.len - 1));
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) emit_defers(cg, scopes, *depth, func_root);
		nt_vec_free(&scopes[*depth].defers); nt_vec_free(&scopes[*depth].vars); (*depth)--; break;
	}
	case NT_S_TRY: {
		fun_sig *sz_fn = lookup_fun(cg, "rt_jmpbuf_size");
		fun_sig *set_env = lookup_fun(cg, "rt_set_panic_env");
		fun_sig *clr_env = lookup_fun(cg, "rt_clear_panic_env");
		fun_sig *get_err = lookup_fun(cg, "rt_get_panic_val");
		if (!sz_fn || !set_env || !clr_env || !get_err) {
			fprintf(stderr, "Error: missing rt try functions\n");
			cg->had_error = 1;
			return;
		}
		LLVMValueRef sz_val = LLVMBuildCall2(cg->builder, sz_fn->type, sz_fn->value, NULL, 0, "");
		LLVMValueRef jmpbuf = LLVMBuildArrayAlloca(cg->builder, LLVMInt8TypeInContext(cg->ctx), sz_val, "jmpbuf");
		LLVMValueRef jmpbuf_ptr = LLVMBuildPtrToInt(cg->builder, jmpbuf, cg->type_i64, "");
		LLVMBuildCall2(cg->builder, set_env->type, set_env->value, (LLVMValueRef[]){jmpbuf_ptr}, 1, "");
		LLVMValueRef setjmp_func = LLVMGetNamedFunction(cg->module, "_setjmp");
		if (!setjmp_func) setjmp_func = LLVMGetNamedFunction(cg->module, "setjmp");
		if (!setjmp_func) {
			 LLVMTypeRef arg_t = LLVMPointerTypeInContext(cg->ctx, 0);
			 LLVMTypeRef ret_t = LLVMInt32TypeInContext(cg->ctx);
			 setjmp_func = LLVMAddFunction(cg->module, "setjmp", LLVMFunctionType(ret_t, &arg_t, 1, 0));
		}
		LLVMValueRef sj_res = LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(setjmp_func), setjmp_func, (LLVMValueRef[]){jmpbuf}, 1, "sj_res");
		LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
		LLVMBasicBlockRef try_b = LLVMAppendBasicBlockInContext(cg->ctx, func, "try_body");
		LLVMBasicBlockRef catch_b = LLVMAppendBasicBlockInContext(cg->ctx, func, "catch_body");
		LLVMBasicBlockRef end_b = LLVMAppendBasicBlockInContext(cg->ctx, func, "try_end");
		LLVMBuildCondBr(cg->builder, LLVMBuildICmp(cg->builder, LLVMIntEQ, sj_res, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false), ""), try_b, catch_b);
		LLVMPositionBuilderAtEnd(cg->builder, try_b);
		gen_stmt(cg, scopes, depth, s->as.tr.body, func_root, is_tail);
		LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, end_b);
		LLVMPositionBuilderAtEnd(cg->builder, catch_b);
		LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
		LLVMValueRef err_val = LLVMBuildCall2(cg->builder, get_err->type, get_err->value, NULL, 0, "err");
		if (s->as.tr.err) {
			 (*depth)++; scopes[*depth].vars.len = scopes[*depth].vars.cap = 0; scopes[*depth].vars.data = NULL; scopes[*depth].defers.len = scopes[*depth].defers.cap = 0; scopes[*depth].defers.data = NULL; scopes[*depth].break_bb = scopes[*depth-1].break_bb; scopes[*depth].continue_bb = scopes[*depth-1].continue_bb;
			 LLVMValueRef err_var = build_alloca(cg, s->as.tr.err);
			 LLVMBuildStore(cg->builder, err_val, err_var);
			 bind(scopes, *depth, s->as.tr.err, err_var, NULL);
			 gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
			 nt_vec_free(&scopes[*depth].defers); nt_vec_free(&scopes[*depth].vars); (*depth)--;
		} else {
			 gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
		}
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildBr(cg->builder, end_b);
		LLVMPositionBuilderAtEnd(cg->builder, end_b);
		break;
	}
	case NT_S_FUNC: gen_func(cg, s, s->as.fn.name, scopes, *depth, NULL); break;
	case NT_S_DEFER: if (s->as.de.body) nt_vec_push(&scopes[*depth].defers, s->as.de.body); break;
	case NT_S_BREAK: { LLVMBasicBlockRef db = NULL; for (ssize_t i=(ssize_t)*depth; i>=0; i--) if (scopes[i].break_bb) { db = scopes[i].break_bb; break; } if (db) LLVMBuildBr(cg->builder, db); break; }
	case NT_S_CONTINUE: { LLVMBasicBlockRef db = NULL; for (ssize_t i=(ssize_t)*depth; i>=0; i--) if (scopes[i].continue_bb) { db = scopes[i].continue_bb; break; } if (db) LLVMBuildBr(cg->builder, db); break; }
	case NT_S_LAYOUT: {
		size_t off = 0; for (size_t i=0; i<s->as.layout.fields.len; i++) {
			char buf[128]; snprintf(buf, sizeof(buf), "%s.%s", s->as.layout.name, s->as.layout.fields.data[i].name);
			LLVMValueRef fv = LLVMAddFunction(cg->module, buf, LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64}, 1, 0));
			LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(fv, "e"));
			LLVMBuildRet(cg->builder, LLVMBuildAdd(cg->builder, LLVMGetParam(fv, 0), LLVMConstInt(cg->type_i64, (uint64_t)off, false), "")); off += (size_t)s->as.layout.fields.data[i].width;
		}
		break;
	}
	case NT_S_MODULE: {
		for (size_t i = 0; i < s->as.module.body.len; ++i) {
			gen_stmt(cg, scopes, depth, s->as.module.body.data[i], func_root, is_tail);
		}
		break;
	}
	case NT_S_EXPORT: break;
	default: break;
	}
}

static void gen_func(nt_codegen *cg, nt_stmt *fn, const char *name, scope *scopes, size_t depth, binding_list *captures) {
	if (!fn->as.fn.body) return;
	LLVMValueRef f = LLVMGetNamedFunction(cg->module, name);
	if (!f) {
		size_t n_params = fn->as.fn.params.len;
		// If captures pointer is non-null, this is a closure/lambda context, so we MUST accept 'env' param.
		size_t total_args = captures ? n_params + 1 : n_params;
		LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * total_args);
		for (size_t i=0; i<total_args; i++) pt[i] = cg->type_i64;
		LLVMTypeRef ft = LLVMFunctionType(cg->type_i64, pt, (unsigned)total_args, 0);
		f = LLVMAddFunction(cg->module, name, ft);
		// Store explicit params count for callers
		fun_sig sig = { .name = strdup(name), .type = ft, .value = f, .stmt = fn, .arity = (int)n_params, .is_variadic = fn->as.fn.is_variadic };
		nt_vec_push(&cg->fun_sigs, sig);
	} else {
		// Overwrite: remove existing basic blocks if any
		LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f);
		while (bb) {
			LLVMBasicBlockRef next = LLVMGetNextBasicBlock(bb);
			LLVMDeleteBasicBlock(bb);
			bb = next;
		}
	}
	LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
	LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(f, "entry"));
	size_t fd = depth + 1; size_t root = fd;
	// Init scope
	scopes[fd].vars.len = scopes[fd].vars.cap = 0; scopes[fd].vars.data = NULL;
	scopes[fd].defers.len = scopes[fd].defers.cap = 0; scopes[fd].defers.data = NULL;
	size_t param_offset = 0;
	if (captures) {
		param_offset = 1;
		LLVMValueRef env_arg = LLVMGetParam(f, 0);
		LLVMValueRef env_raw = LLVMBuildIntToPtr(cg->builder, env_arg, LLVMPointerType(cg->type_i64, 0), "env_raw");
		for (size_t i=0; i<captures->len; i++) {
			 LLVMValueRef src = LLVMBuildGEP2(cg->builder, cg->type_i64, env_raw, (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1, "");
			 LLVMValueRef val = LLVMBuildLoad2(cg->builder, cg->type_i64, src, "");
			 // For closures, we copy captures into local variables of the new scope
			 // Note: Bind to the captured name
			 LLVMValueRef lv = build_alloca(cg, captures->data[i].name);
			 LLVMBuildStore(cg->builder, val, lv);
			 bind(scopes, fd, captures->data[i].name, lv, NULL);
		}
	}

	for (size_t i=0; i<fn->as.fn.params.len; i++) {
		LLVMValueRef a = build_alloca(cg, fn->as.fn.params.data[i].name);
		LLVMBuildStore(cg->builder, LLVMGetParam(f, (unsigned)(i + param_offset)), a);
		bind(scopes, fd, fn->as.fn.params.data[i].name, a, NULL);
	}
	size_t old_root = cg->func_root;
	cg->func_root = root;
	gen_stmt(cg, scopes, &fd, fn->as.fn.body, root, true);
	cg->func_root = old_root;
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
	nt_vec_free(&scopes[root].defers); nt_vec_free(&scopes[root].vars);
	if (cur) LLVMPositionBuilderAtEnd(cg->builder, cur);
}

void nt_codegen_init_with_context(nt_codegen *cg, nt_program *prog, LLVMModuleRef mod, LLVMContextRef ctx, LLVMBuilderRef builder) {
	memset(cg, 0, sizeof(nt_codegen));
	cg->ctx = ctx; cg->module = mod; cg->builder = builder; cg->prog = prog;
	cg->fun_sigs.len = cg->fun_sigs.cap = 0; cg->fun_sigs.data = NULL;
	cg->global_vars.len = cg->global_vars.cap = 0; cg->global_vars.data = NULL;
	cg->interns.len = cg->interns.cap = 0; cg->interns.data = NULL; cg->llvm_ctx_owned = false;
	cg->import_aliases.len = cg->import_aliases.cap = 0; cg->import_aliases.data = NULL;
	cg->is_comptime = false;
	cg->type_i64 = LLVMInt64TypeInContext(cg->ctx); cg->had_error = 0; cg->lambda_count = 0; add_builtins(cg);
}

void nt_codegen_init(nt_codegen *cg, nt_program *prog, const char *name) {
	memset(cg, 0, sizeof(nt_codegen));
	LLVMInitializeNativeTarget(); LLVMInitializeNativeAsmPrinter();
	cg->ctx = LLVMContextCreate(); cg->llvm_ctx_owned = true; cg->module = LLVMModuleCreateWithNameInContext(name, cg->ctx); cg->builder = LLVMCreateBuilderInContext(cg->ctx); cg->prog = prog;
	cg->type_i64 = LLVMInt64TypeInContext(cg->ctx);
	add_builtins(cg);
	LLVMAddGlobal(cg->module, cg->type_i64, "__NYTRIX__");
}

static void collect_sigs(nt_codegen *cg, nt_stmt *s) {
	if (s->kind == NT_S_FUNC) {
		/* DEBUG */
		LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * s->as.fn.params.len);
		for (size_t j=0; j<s->as.fn.params.len; j++) pt[j] = cg->type_i64;
		LLVMTypeRef ft = LLVMFunctionType(cg->type_i64, pt, (unsigned)s->as.fn.params.len, 0);
		LLVMValueRef f = LLVMGetNamedFunction(cg->module, s->as.fn.name);
		if (!f) f = LLVMAddFunction(cg->module, s->as.fn.name, ft);
		LLVMSetAlignment(f, 16);
		fun_sig sig = { .name = strdup(s->as.fn.name), .type = ft, .value = f, .stmt = s, .arity = (int)s->as.fn.params.len, .is_variadic = s->as.fn.is_variadic };
		nt_vec_push(&cg->fun_sigs, sig);
	} else if (s->kind == NT_S_VAR) {
		for (size_t j=0; j<s->as.var.names.len; j++) {
			const char *n = s->as.var.names.data[j];
			// Use simple exact lookup here to see if we already created this global
			bool found = false;
			for (size_t k=0; k<cg->global_vars.len; k++) {
				if (strcmp(cg->global_vars.data[k].name, n) == 0) { found = true; break; }
			}
			if (!found) {
				LLVMValueRef g = LLVMAddGlobal(cg->module, cg->type_i64, n);
				LLVMSetInitializer(g, LLVMConstInt(cg->type_i64, 0, false));
				binding b = { strdup(n), g, NULL };
				nt_vec_push(&cg->global_vars, b);
			}
		}
	} else if (s->kind == NT_S_MODULE) {
		for (size_t i=0; i<s->as.module.body.len; i++) collect_sigs(cg, s->as.module.body.data[i]);
	}
}

static void add_import_alias(nt_codegen *cg, const char *alias, const char *full_name) {
	if (!alias || !*alias || !full_name || !*full_name) return;
	// fprintf(stderr, "DEBUG: add_import_alias alias=%s full=%s\n", alias, full_name);
	for (size_t i = 0; i < cg->import_aliases.len; ++i) {
		if (cg->import_aliases.data[i].name && strcmp(cg->import_aliases.data[i].name, alias) == 0) return;
	}
	binding alias_bind = {0};
	alias_bind.name = strdup(alias);
	alias_bind.stmt = (nt_stmt *)strdup(full_name);
	nt_vec_push(&cg->import_aliases, alias_bind);
}

static void add_import_alias_from_full(nt_codegen *cg, const char *full_name) {
	if (!full_name || !*full_name) return;
	const char *last_dot = strrchr(full_name, '.');
	const char *alias = last_dot ? last_dot + 1 : full_name;
	add_import_alias(cg, alias, full_name);
}

static nt_stmt *find_module_stmt(nt_stmt *s, const char *name) {
	if (!s || !name) return NULL;
	if (s->kind == NT_S_MODULE && s->as.module.name && strcmp(s->as.module.name, name) == 0) {
		return s;
	}
	if (s->kind == NT_S_MODULE) {
		for (size_t i = 0; i < s->as.module.body.len; ++i) {
			nt_stmt *found = find_module_stmt(s->as.module.body.data[i], name);
			if (found) return found;
		}
	}
	return NULL;
}

static bool module_has_export_list(const nt_stmt *mod) {
	if (!mod || mod->kind != NT_S_MODULE) return false;
	for (size_t i = 0; i < mod->as.module.body.len; ++i) {
		if (mod->as.module.body.data[i]->kind == NT_S_EXPORT) return true;
	}
	return false;
}

static void collect_module_exports(nt_stmt *mod, str_list *exports) {
	if (!mod || mod->kind != NT_S_MODULE) return;
	const char *mod_name = mod->as.module.name;
	for (size_t i = 0; i < mod->as.module.body.len; ++i) {
		nt_stmt *child = mod->as.module.body.data[i];
		if (child->kind != NT_S_EXPORT) continue;
		for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
			const char *name = child->as.exprt.names.data[j];
			if (!name) continue;
			char *full = NULL;
			if (strchr(name, '.')) {
				full = strdup(name);
			} else {
				size_t len = strlen(mod_name) + 1 + strlen(name) + 1;
				full = malloc(len);
				snprintf(full, len, "%s.%s", mod_name, name);
			}
			nt_vec_push(exports, full);
		}
	}
}

static void collect_module_defs(nt_stmt *mod, str_list *exports) {
	if (!mod || mod->kind != NT_S_MODULE) return;
	for (size_t i = 0; i < mod->as.module.body.len; ++i) {
		nt_stmt *child = mod->as.module.body.data[i];
		if (child->kind == NT_S_FUNC) {
			nt_vec_push(exports, strdup(child->as.fn.name));
		} else if (child->kind == NT_S_VAR) {
			for (size_t j = 0; j < child->as.var.names.len; ++j) {
				nt_vec_push(exports, strdup(child->as.var.names.data[j]));
			}
		}
	}
}

static void add_imports_from_prefix(nt_codegen *cg, const char *mod) {
	if (!mod || !*mod) return;
	size_t mod_len = strlen(mod);
	for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
		const char *name = cg->fun_sigs.data[i].name;
		if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
			add_import_alias_from_full(cg, name);
		}
	}
	for (size_t i = 0; i < cg->global_vars.len; ++i) {
		const char *name = cg->global_vars.data[i].name;
		if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
			add_import_alias_from_full(cg, name);
		}
	}
}

static void process_use_imports(nt_codegen *cg, nt_stmt *s) {
	if (s->kind == NT_S_USE) {
		if (!s->as.use.import_all && s->as.use.imports.len == 0) return;
		const char *mod = s->as.use.module;
		if (s->as.use.imports.len > 0) {
			for (size_t i = 0; i < s->as.use.imports.len; ++i) {
				nt_use_item *item = &s->as.use.imports.data[i];
				if (!item->name) continue;
				size_t len = strlen(mod) + 1 + strlen(item->name) + 1;
				char *full = malloc(len);
				snprintf(full, len, "%s.%s", mod, item->name);

				add_import_alias(cg, item->alias ? item->alias : item->name, full);
				free(full);
			}
			return;
		}
		if (s->as.use.import_all) {
			str_list exports = {0};
			bool has_export_list = false;
			nt_stmt *mod_stmt = NULL;
			for (size_t i = 0; i < cg->prog->body.len; ++i) {
				mod_stmt = find_module_stmt(cg->prog->body.data[i], mod);
				if (mod_stmt) break;
			}
			if (mod_stmt) {
				has_export_list = module_has_export_list(mod_stmt);
				if (has_export_list) {
					collect_module_exports(mod_stmt, &exports);
				}
				if (!has_export_list || mod_stmt->as.module.export_all) {
					collect_module_defs(mod_stmt, &exports);
				}
			}
			if (!mod_stmt || exports.len == 0) {
				add_imports_from_prefix(cg, mod);
			} else {
				for (size_t i = 0; i < exports.len; ++i) {
					add_import_alias_from_full(cg, exports.data[i]);
					free(exports.data[i]);
				}
				nt_vec_free(&exports);
			}
			return;
		}
	} else if (s->kind == NT_S_MODULE) {
		for (size_t i = 0; i < s->as.module.body.len; ++i) {
			process_use_imports(cg, s->as.module.body.data[i]);
		}
	}
}

static void collect_use_aliases(nt_codegen *cg, nt_stmt *s) {
	if (s->kind == NT_S_USE) {
		if (s->as.use.import_all || s->as.use.imports.len > 0) return;
		const char *alias = s->as.use.alias;
		if (!alias) {
			// Infer alias from module path (last component)
			const char *mod = s->as.use.module;
			const char *dot = strrchr(mod, '.');
			alias = dot ? dot + 1 : mod;
		}
		binding alias_bind = {0};
		alias_bind.name = strdup(alias);
		alias_bind.stmt = (nt_stmt*)strdup(s->as.use.module);
		// Handle specific imports list: use Mod (a, b as c)
		for (size_t i = 0; i < s->as.use.imports.len; ++i) {
			nt_use_item item = s->as.use.imports.data[i];
			const char *target = item.name;
			const char *item_alias = item.alias ? item.alias : item.name;
			// Maps alias -> Module.target
			binding import_bind = {0};
			import_bind.name = strdup(item_alias);
			char *full_target = malloc(strlen(s->as.use.module) + 1 + strlen(target) + 1);
			sprintf(full_target, "%s.%s", s->as.use.module, target);
			import_bind.stmt = (nt_stmt*)full_target;
			nt_vec_push(&cg->import_aliases, import_bind);
		}
		nt_vec_push(&cg->aliases, alias_bind);
	} else if (s->kind == NT_S_MODULE) {
		for (size_t i=0; i<s->as.module.body.len; i++) collect_use_aliases(cg, s->as.module.body.data[i]);
	}
}

static void collect_use_modules(nt_codegen *cg, nt_stmt *s) {
	if (s->kind == NT_S_USE) {
		if (s->as.use.import_all || s->as.use.imports.len > 0) return;
		const char *mod = s->as.use.module;
		if (mod && *mod && s->as.use.import_all) {
			for (size_t i = 0; i < cg->use_modules.len; ++i) {
				if (strcmp(cg->use_modules.data[i], mod) == 0) return;
			}
			nt_vec_push(&cg->use_modules, strdup(mod));
		}
	} else if (s->kind == NT_S_MODULE) {
		for (size_t i=0; i<s->as.module.body.len; i++) collect_use_modules(cg, s->as.module.body.data[i]);
	}
}

static void emit_top_functions(nt_codegen *cg, nt_stmt *s, scope *gsc, size_t gd, const char *cur_mod) {
	if (s->kind == NT_S_FUNC) {
		cg->current_mod = cur_mod;
		gen_func(cg, s, s->as.fn.name, gsc, gd, NULL);
	} else if (s->kind == NT_S_MODULE) {
		for (size_t i=0; i<s->as.module.body.len; i++) emit_top_functions(cg, s->as.module.body.data[i], gsc, gd, s->as.module.name);
	}
}

static void process_exports(nt_codegen *cg, nt_stmt *s) {
	if (s->kind == NT_S_MODULE) {
		const char *mod_name = s->as.module.name;
		for (size_t i = 0; i < s->as.module.body.len; ++i) {
			nt_stmt *child = s->as.module.body.data[i];
			if (child->kind == NT_S_EXPORT) {
				for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
					const char *target = child->as.exprt.names.data[j];
					char alias[256];
					snprintf(alias, sizeof(alias), "%s.%s", mod_name, target);
					char full_target[256];
					snprintf(full_target, sizeof(full_target), "%s.%s", mod_name, target);
					fun_sig *fs = lookup_fun(cg, full_target);
					if (!fs) fs = lookup_fun(cg, target);
					if (fs) {
						fun_sig new_sig = *fs;
						new_sig.name = strdup(alias);
						nt_vec_push(&cg->fun_sigs, new_sig);
					} else {
						binding *gb = lookup_global(cg, full_target);
						if (!gb) gb = lookup_global(cg, target);
						if (gb) {
							binding new_bind = *gb;
							new_bind.name = strdup(alias);
							nt_vec_push(&cg->global_vars, new_bind);
						}
					}
				}
			} else if (child->kind == NT_S_MODULE) {
				// Recurse? Though nesting modules resets name context in parser currently,
				// so checking submodule is valid recursively.
				process_exports(cg, child);
			}
		}
	}
}

void nt_codegen_emit(nt_codegen *cg) {
	scope gsc[64] = {0}; size_t gd = 0;
	// Collect module aliases and use-modules before function bodies are emitted
	for (size_t i=0; i<cg->prog->body.len; i++) {
		collect_use_aliases(cg, cg->prog->body.data[i]);
		collect_use_modules(cg, cg->prog->body.data[i]);
	}
	// First pass: collect all signatures (including nested modules)
	for (size_t i=0; i<cg->prog->body.len; i++) {
		nt_stmt *s = cg->prog->body.data[i];
		collect_sigs(cg, s);
	}
	// Process exports to create aliases
	for (size_t i=0; i<cg->prog->body.len; i++) {
		process_exports(cg, cg->prog->body.data[i]);
	}
	// Process explicit imports after sigs/exports are known
	for (size_t i=0; i<cg->prog->body.len; i++) {
		process_use_imports(cg, cg->prog->body.data[i]);
	}
	// Second pass: emit function bodies
	for (size_t i=0; i<cg->prog->body.len; i++) {
		emit_top_functions(cg, cg->prog->body.data[i], gsc, gd, NULL);
	}
}

LLVMValueRef nt_codegen_emit_script(nt_codegen *cg, const char *name) {
	cg->current_mod = NULL;
	LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
	if (fn) return fn;
	fn = LLVMAddFunction(cg->module, name, LLVMFunctionType(cg->type_i64, NULL, 0, 0));
	LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
	// Create two blocks: init (for internal setup) and body (for user code)
	LLVMBasicBlockRef init_block = LLVMAppendBasicBlock(fn, "init");
	LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(fn, "body");
	// 1. Generate user code first (into body_block) to discover all strings
	LLVMPositionBuilderAtEnd(cg->builder, body_block);
	scope sc[64] = {0}; size_t d = 0;
	for (size_t i=0; i<cg->prog->body.len; i++) {
		if (cg->prog->body.data[i]->kind != NT_S_FUNC) {
			gen_stmt(cg, sc, &d, cg->prog->body.data[i], 0, false);
		}
	}
	// If the user code didn't terminate, add a return logic
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
		LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
	}
	// 2. Now fill the init block
	LLVMPositionBuilderAtEnd(cg->builder, init_block);
	// Initialize ALL runtime string pointers found
	LLVMTypeRef i8_ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
	LLVMTypeRef asm_func_ty = LLVMFunctionType(i8_ptr_ty, (LLVMTypeRef[]){i8_ptr_ty}, 1, false);
	LLVMValueRef identity_asm = LLVMConstInlineAsm(asm_func_ty, "", "=r,0", true, false);
	for (size_t i = 0; i < cg->interns.len; i++) {
		LLVMValueRef str_array_global = cg->interns.data[i].gv;
		LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
		if (!str_array_global || !runtime_ptr_global) continue;
		if (LLVMGetTypeKind(LLVMTypeOf(runtime_ptr_global)) != LLVMPointerTypeKind) continue;
		// 1. Bitcast global to i8*
		LLVMValueRef global_i8_ptr = LLVMBuildBitCast(cg->builder, str_array_global, i8_ptr_ty, "");
		// 2. Pass through identity ASM to prevent constant folding
		LLVMValueRef runtime_base = LLVMBuildCall2(cg->builder, asm_func_ty, identity_asm, (LLVMValueRef[]){global_i8_ptr}, 1, "");
		// 3. GEP offset 64 bytes
		LLVMValueRef indices[] = { LLVMConstInt(cg->type_i64, 64, 0) };
		LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(cg->builder, LLVMInt8TypeInContext(cg->ctx), runtime_base, indices, 1, "");
		// 4. Convert to int and store
		LLVMValueRef str_data_int = LLVMBuildPtrToInt(cg->builder, str_data_ptr, cg->type_i64, "");
		LLVMBuildStore(cg->builder, str_data_int, runtime_ptr_global);
	}
	// Jump from init to body
	LLVMBuildBr(cg->builder, body_block);
	// Cleanup
	nt_vec_free(&sc[0].defers); nt_vec_free(&sc[0].vars);
	// Restore builder to end of function (just in case caller expects it)
	if (cur) {
		LLVMPositionBuilderAtEnd(cg->builder, cur);
	}
	return fn;
}

void nt_codegen_dispose(nt_codegen *cg) {
	LLVMDisposeBuilder(cg->builder);
	if (cg->llvm_ctx_owned) {
		if (cg->module) LLVMDisposeModule(cg->module);
		LLVMContextDispose(cg->ctx);
	}
	for (size_t i=0; i<cg->fun_sigs.len; i++) {
		free((void*)cg->fun_sigs.data[i].name);
	}
	for (size_t i=0; i<cg->global_vars.len; i++) {
		free((void*)cg->global_vars.data[i].name);
	}
	for (size_t i=0; i<cg->interns.len; i++) {
		free(cg->interns.data[i].alloc);
	}
	for (size_t i=0; i<cg->aliases.len; i++) {
		free((void*)cg->aliases.data[i].name);
		free((void*)cg->aliases.data[i].stmt);
	}
	for (size_t i=0; i<cg->import_aliases.len; i++) {
		free((void*)cg->import_aliases.data[i].name);
		free((void*)cg->import_aliases.data[i].stmt);
	}
	for (size_t i=0; i<cg->use_modules.len; i++) {
		free((void*)cg->use_modules.data[i]);
	}
	nt_vec_free(&cg->fun_sigs);
	nt_vec_free(&cg->global_vars);
	nt_vec_free(&cg->interns);
	nt_vec_free(&cg->aliases);
	nt_vec_free(&cg->import_aliases);
	nt_vec_free(&cg->use_modules);
}

void nt_codegen_reset(nt_codegen *cg) { (void)cg; }
