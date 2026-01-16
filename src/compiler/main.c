#include "codegen.h"
#include "std_loader.h"
#include "util.h"
#include "driver/builder.h"
#include "codegen/llvm_emit.h"
#include "runtime/jit_symbols.h"

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/Support.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <execinfo.h>
#include <signal.h>
#include <dlfcn.h>
#include <unistd.h>

// REPL hooks
void nt_repl_run(int opt_level, const char *opt_pipeline, const char *init_code);
void nt_repl_set_std_mode(nt_std_mode mode);
void nt_repl_set_plain(int plain);
static void nytrix_ensure_aot_entry(nt_codegen *cg, LLVMValueRef script_fn);
void rt_cleanup_args(void);

static void handle_segv(int sig) {
	void *bt[64];
	int n = backtrace(bt, 64);
	fprintf(stderr, "Caught signal %d, backtrace:\n", sig);
	backtrace_symbols_fd(bt, n, STDERR_FILENO);
	_exit(128 + sig);
}

// Simple 64-bit FNV-1a hash for cache keys (now in util.h, kept wrapper/static if needed or just use nt_fnv1a64)
// static uint64_t fnv1a64(...) replaced by nt_fnv1a64

static void append_use(char ***uses, size_t *len, size_t *cap, const char *name) {
	for (size_t i = 0; i < *len; ++i) {
		if (strcmp((*uses)[i], name) == 0) return;
	}
	if (*len == *cap) {
		size_t new_cap = *cap ? (*cap * 2) : 8;
		char **tmp = realloc(*uses, new_cap * sizeof(char *));
		if (!tmp) { fprintf(stderr, "oom\n"); exit(1); }
		*uses = tmp;
		*cap = new_cap;
	}
	(*uses)[(*len)++] = strdup(name);
}

static int is_std_or_lib_use(const char *name) {
	return name && (
		strcmp(name, "std") == 0 ||
		strcmp(name, "lib") == 0 ||
		strncmp(name, "std.", 4) == 0 ||
		strncmp(name, "lib.", 4) == 0
	);
}

static void append_std_prelude(char ***uses, size_t *len, size_t *cap) {
	size_t prelude_count = 0;
	const char **prelude = nt_std_prelude(&prelude_count);
	for (size_t i = 0; i < prelude_count; ++i) {
		append_use(uses, len, cap, prelude[i]);
	}
}

static void free_use_list(char **uses, size_t count) {
	if (!uses) return;
	for (size_t i = 0; i < count; ++i) free(uses[i]);
	free(uses);
}

typedef struct nytrix_cleanup_state {
	nt_codegen *cg;
	int cg_initialized;
	nt_program *prog;
	nt_arena *arena;
	char *std_src;
	char *user_src;
	char *source;
	char **uses;
	size_t use_count;
} nytrix_cleanup_state;

static nytrix_cleanup_state g_cleanup = {0};
static int g_cleanup_done = 0;

static void nytrix_cleanup_global(void) {
	if (g_cleanup_done) return;
	g_cleanup_done = 1;
	// fprintf(stderr, "DEBUG: Running cleanup. Prog: %p, Arena: %p\n", g_cleanup.prog, g_cleanup.arena);
	if (g_cleanup.cg && g_cleanup.cg_initialized) nt_codegen_dispose(g_cleanup.cg);
	if (g_cleanup.arena) {
		if (g_cleanup.prog) nt_program_free(g_cleanup.prog, g_cleanup.arena);
		else { nt_arena_free(g_cleanup.arena); free(g_cleanup.arena); }
	}
	if (g_cleanup.std_src) free(g_cleanup.std_src);
	if (g_cleanup.uses) free_use_list(g_cleanup.uses, g_cleanup.use_count);
	if (g_cleanup.user_src) free(g_cleanup.user_src);
	if (g_cleanup.source) free(g_cleanup.source);
	rt_cleanup_args();
	memset(&g_cleanup, 0, sizeof(g_cleanup));
}

static char *dup_string_token(nt_token t) {
	if (t.len < 2) return NULL;
	size_t head = 1, tail = 1;
	if (t.len >= 6 && t.lexeme[0] == t.lexeme[1] && t.lexeme[1] == t.lexeme[2]) {
		head = 3;
		tail = 3;
	}
	if (t.len < head + tail) return NULL;
	size_t out_len = t.len - head - tail;
	char *out = malloc(out_len + 1);
	if (!out) return NULL;
	memcpy(out, t.lexeme + head, out_len);
	out[out_len] = '\0';
	return out;
}

static char *parse_use_name(nt_lexer *lx, nt_token *entry_tok, nt_token *out_last_tok) {
	nt_token t = *entry_tok;
	if (t.kind == NT_T_STRING) {
		char *name = dup_string_token(t);
		if (out_last_tok) *out_last_tok = nt_lex_next(lx);
		return name;
	}
	if (t.kind != NT_T_IDENT) return NULL;
	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	if (!buf) return NULL;
	memcpy(buf, t.lexeme, t.len);
	len += t.len;
	for (;;) {
		nt_token tok = nt_lex_next(lx);
		if (tok.kind == NT_T_DOT) {
			nt_token id = nt_lex_next(lx);
			if (id.kind != NT_T_IDENT) { free(buf); return NULL; }
			if (len + 1 + id.len + 1 > cap) {
				cap = (len + 1 + id.len + 1) * 2;
				char *nb = realloc(buf, cap);
				if (!nb) { free(buf); return NULL; }
				buf = nb;
			}
			buf[len++] = '.';
			memcpy(buf + len, id.lexeme, id.len);
			len += id.len;
		} else {
			if (out_last_tok) *out_last_tok = tok;
			break;
		}
	}
	buf[len] = '\0';
	return buf;
}

static char **collect_use_modules(const char *src, size_t *out_count) {
	nt_lexer lx;
	nt_lexer_init(&lx, src, "<collect_use>");
	int depth = 0;
	char **uses = NULL;
	size_t len = 0, cap = 0;

	nt_token t = nt_lex_next(&lx);
	for (;;) {
		if (t.kind == NT_T_EOF) break;
		if (t.kind == NT_T_LBRACE) {
			depth++;
			t = nt_lex_next(&lx);
		}
		else if (t.kind == NT_T_RBRACE) {
			if (depth > 0) depth--;
			t = nt_lex_next(&lx);
		}
		else if (t.kind == NT_T_USE && depth == 0) {
			t = nt_lex_next(&lx); // consume USE, get ident
			nt_token next_tok;
			char *name = parse_use_name(&lx, &t, &next_tok);
			if (!name) {
				fprintf(stderr, "error: invalid use statement\n");
				exit(1);
			}
			append_use(&uses, &len, &cap, name);
			free(name);
			t = next_tok; // Continue with the token returned by parse_use_name
		} else {
			t = nt_lex_next(&lx);
		}
	}
	if (out_count) *out_count = len;
	return uses;
}

static void usage(const char *prog) {
	fprintf(stderr, "\n%sNytrix Compiler%s - Small core with stdlib in .ny\n\n", nt_clr(NT_CLR_BOLD NT_CLR_CYAN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sUSAGE:%s %s [OPTIONS] file.ny\n\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET), prog);
	fprintf(stderr, "%sOPTIMIZATION:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-O1/-O2/-O3%s        Optimization level (default: -O0)\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-passes=PIPE%s       Custom LLVM pass pipeline (e.g., 'default<O2>')\n\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sEXECUTION:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-run%s               JIT execute main() after compilation\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-emit-only%s         Only emit IR, don't execute (default)\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-o [path]%s          Emit ELF at [path] (default: a.out; implies -emit-only)\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s--output=<path>%s    Same as -o\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-c <code>%s          Execute inline code\n\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sREPL:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-i, -interactive%s   Interactive REPL with readline\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-repl%s              Read source from stdin (one-shot)\n\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sDEBUGGING:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-v, -verbose%s       Show compilation steps\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s--debug%s            Enable verbose + debug logs\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-vv, -vvv%s          Increased verbosity levels\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-time%s              Show timing for each phase\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-ast%s          Dump parsed AST\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-llvm%s         Print LLVM IR to stdout\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-tokens%s       Dump lexer tokens\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-docs%s         Extract and print function docstrings\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-funcs%s        List all compiled functions\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-symbols%s      Show runtime symbol table\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-dump-stats%s        Print compilation statistics\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-verify%s            Verify LLVM module\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s--dump-on-error%s    Write build/debug/last_source.ny and last_ir.ll on errors\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-trace%s             Enable execution tracing\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-std%s               Include full stdlib bundle\n", nt_clr(NT_CLR_MAGENTA), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-no-std%s            Don't include stdlib\n", nt_clr(NT_CLR_MAGENTA), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s--std=MODE%s         MODE: none | prelude | lazy | full | use:mod1,mod2\n", nt_clr(NT_CLR_MAGENTA), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s--color=WHEN%s       WHEN: auto | always | never\n", nt_clr(NT_CLR_MAGENTA), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-safe-mode%s         Enable all safety checks\n\n", nt_clr(NT_CLR_MAGENTA), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sINFO:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-h, -help, --help%s  Show this help message\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s-version%s           Show version info\n\n", nt_clr(NT_CLR_BLUE), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sENVIRONMENT:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %sNYTRIX_RUN%s         Same as -run flag\n", nt_clr(NT_CLR_YELLOW), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %sNYTRIX_DUMP_TOKENS%s Dump tokens during lexing\n\n", nt_clr(NT_CLR_YELLOW), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "%sEXAMPLES:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s%s examples/quicksort.ny              # compile only%s\n", nt_clr(NT_CLR_CYAN), prog, nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s%s -O2 -run examples/quicksort.ny     # compile & run optimized%s\n", nt_clr(NT_CLR_CYAN), prog, nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s%s -v -time -verify examples/file.ny  # debug compilation%s\n", nt_clr(NT_CLR_CYAN), prog, nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s%s -c 'print(\"hello\")'                # run inline%s\n", nt_clr(NT_CLR_CYAN), prog, nt_clr(NT_CLR_RESET));
	fprintf(stderr, "  %s%s -i                                 # interactive REPL%s\n", nt_clr(NT_CLR_CYAN), prog, nt_clr(NT_CLR_RESET));
	exit(1);
}

static char **parse_use_list(const char *spec, size_t *out_count) {
	size_t len = 0, cap = 8;
	char **list = malloc(sizeof(char *) * cap);
	if (!list) return NULL;
	const char *p = spec;
	while (*p) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		const char *start = p;
		while (*p && *p != ',') p++;
		const char *end = p;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
		size_t n = (size_t)(end - start);
		if (n > 0) {
			if (len + 1 > cap) {
				cap *= 2;
				list = realloc(list, sizeof(char *) * cap);
				if (!list) return NULL;
			}
			char *s = malloc(n + 1);
			memcpy(s, start, n);
			s[n] = '\0';
			list[len++] = s;
		}
		if (*p == ',') p++;
	}
	if (out_count) *out_count = len;
	return list;
}

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
static char *get_executable_dir(void) {
	static char buf[PATH_MAX];
	if (buf[0]) return buf;
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len == -1) return NULL;
	buf[len] = '\0';
	char *slash = strrchr(buf, '/');
	if (slash) *slash = '\0';
	return buf;
}
#else
static char *get_executable_dir(void) { return NULL; }
#endif

static int nytrix_has_sources(const char *root) {
	char probe[8192];
	snprintf(probe, sizeof(probe), "%s/src/compiler/runtime/runtime.c", root);
	return access(probe, R_OK) == 0;
}

static const char *nytrix_src_root(void) {
	static char buf[PATH_MAX];
	if (buf[0]) return buf;
	const char *env = getenv("NYTRIX_ROOT");
	if (env && *env && nytrix_has_sources(env)) {
		snprintf(buf, sizeof(buf), "%s", env);
		return buf;
	}
	char *exe_dir = get_executable_dir();
	if (exe_dir) {
		char tmp[PATH_MAX];
		snprintf(tmp, sizeof(tmp), "%s", exe_dir);
		size_t len = strlen(tmp);
		if (len >= 6 && strcmp(tmp + len - 6, "/build") == 0) {
			tmp[len - 6] = '\0';
		}
		if (nytrix_has_sources(tmp)) {
			snprintf(buf, sizeof(buf), "%s", tmp);
			return buf;
		}
	}
	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd))) {
		char cur[PATH_MAX];
		snprintf(cur, sizeof(cur), "%s", cwd);
		for (;;) {
			if (nytrix_has_sources(cur)) {
				snprintf(buf, sizeof(buf), "%s", cur);
				return buf;
			}
			char *slash = strrchr(cur, '/');
			if (!slash || slash == cur) break;
			*slash = '\0';
		}
	}
	snprintf(buf, sizeof(buf), ".");
	return buf;
}

// Returns a static buffer path if found, or NULL
static const char *resolve_std_bundle(const char *compile_time_path) {
	// 1. Env override
	const char *env = getenv("NYTRIX_STD_PREBUILT");
	if (env && *env && access(env, R_OK) == 0) return env;
	// 2. Relative to executable (Portable / Local build)
	char *exe_dir = get_executable_dir();
	if (exe_dir) {
		static char path[PATH_MAX + 256];
		// Adjacent: ./std_bundle.ny
		snprintf(path, sizeof(path), "%s/std_bundle.ny", exe_dir);
		if (access(path, R_OK) == 0) return path;
		// Installed relative: ../lib/nytrix/std_bundle.ny
		// bin/ny -> lib/nytrix/std_bundle.ny
		snprintf(path, sizeof(path), "%s/../lib/nytrix/std_bundle.ny", exe_dir);
		if (access(path, R_OK) == 0) return path;
		// Installed relative: ../share/nytrix/std_bundle.ny (Common for arch-independent data)
		snprintf(path, sizeof(path), "%s/../share/nytrix/std_bundle.ny", exe_dir);
		if (access(path, R_OK) == 0) return path;
	}
	// 3. Compile-time constant (Global install)
	if (compile_time_path && access(compile_time_path, R_OK) == 0) return compile_time_path;
	// Fallback
	return compile_time_path;
}

static void nytrix_ensure_aot_entry(nt_codegen *cg, LLVMValueRef script_fn) {
	if (!cg || !cg->module || !script_fn) return;
	if (LLVMGetNamedFunction(cg->module, "main")) return;
	LLVMTypeRef script_ty = LLVMTypeOf(script_fn);
	LLVMTypeKind kind = LLVMGetTypeKind(script_ty);
	LLVMTypeRef alias_ty = script_ty;
	if (kind == LLVMFunctionTypeKind) {
		alias_ty = LLVMPointerType(script_ty, 0);
	}
	if (!alias_ty) return;
	LLVMAddAlias2(cg->module, alias_ty, 0, script_fn, "main");
}

int main(int argc, char **argv, char **envp) {

	if (argc < 2) {
		usage(argv[0]);
		return 0;
	}
	signal(SIGSEGV, handle_segv);
	rt_set_args((int64_t)argc, (int64_t)(uintptr_t)argv, (int64_t)(uintptr_t)envp);
	// atexit(nytrix_cleanup_global); // Disabled - causes crashes with JIT
	int opt_level = 0;
	int run_jit = getenv("NYTRIX_RUN") != NULL;
	const char *opt_pipeline = NULL;
	const char *file_path = NULL;
	const char *inline_src = NULL;
	const char *output_path = NULL;
	int dump_docs = 0, repl = 0, interactive = 0;
	int verbose = 0, do_timing = 0, dump_ast = 0, dump_llvm = 0;
	int dump_tokens = getenv("NYTRIX_DUMP_TOKENS") != NULL;
	int dump_on_error = getenv("NYTRIX_DUMP_ON_ERROR") != NULL;
	int dump_funcs = 0, dump_symbols = 0, dump_stats = 0;
	int verify_module = 0, no_std = 0, emit_only = 0, std_full = 0;
	int trace_exec = 0, safe_mode = 0;  // Future features: tracing and sandboxing
	nt_std_mode std_override = NT_STD_NONE;
	nt_std_mode repl_std_override = (nt_std_mode)-1;
	int strip_override = -1; // -1 = env default, 0 = keep symbols, 1 = strip
	int repl_plain = 0;
	int exit_code = 0;
	int has_std_override = 0;
	const char *emit_ir_path = NULL;
	const char *emit_asm_path = NULL;
	char **std_use_list = NULL;
	size_t std_use_count = 0;
	int file_arg_idx = 0; // Track the index of the script file path
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		// Stop parsing if -- is found
		if (strcmp(a, "--") == 0) {
			// If file_path is not set yet, the next arg must be it (if any)
			// But usually -- separates compiler flags from script args
			// If we haven't found a file yet, we might be in a weird state.
			// Let's assume -- stops compiler flag parsing.
			// If file_path is NULL, maybe the next arg is the file?
			// For now, just break. The user script will see -- and subsequent args.
			i++; // Consume the --
			if (i < argc && !file_path) {
				file_path = argv[i];
				file_arg_idx = i;
			}
			break;
		}
		// If we already saw a file path, only allow output flags to appear after it.
		if (file_path && a[0] == '-' &&
			strcmp(a, "-o") != 0 &&
			strcmp(a, "--output") != 0 &&
			strncmp(a, "--output=", 9) != 0) {
			// Treat remaining args as script args.
			break;
		}
		// Optimization
		if (strcmp(a, "-O") == 0 || strcmp(a, "-O2") == 0) opt_level = 2;
		else if (strcmp(a, "-O1") == 0) opt_level = 1;
		else if (strcmp(a, "-O3") == 0) opt_level = 3;
		else if (strcmp(a, "--fast") == 0) { if (!opt_level) opt_level = 2; verify_module = 0; strip_override = 1; }
		else if (strcmp(a, "-O0") == 0) opt_level = 0;
		else if (strncmp(a, "-passes=", 8) == 0) opt_pipeline = a + 8;
		// Execution
		else if (strcmp(a, "-run") == 0) run_jit = 1;
		else if (strcmp(a, "-emit-only") == 0) emit_only = 1;
		else if (strcmp(a, "-o") == 0) {
			if (i + 1 >= argc || argv[i + 1][0] == '-') {
				output_path = "a.out";
			} else {
				output_path = argv[++i];
			}
		}
		else if (strcmp(a, "--output") == 0) {
			if (i + 1 >= argc || argv[i + 1][0] == '-') {
				output_path = "a.out";
			} else {
				output_path = argv[++i];
			}
		}
		else if (strncmp(a, "--output=", 9) == 0) {
			output_path = a + 9;
		}
		else if (strcmp(a, "-c") == 0 && i + 1 < argc) inline_src = argv[++i];
		// REPL
		else if (strcmp(a, "-i") == 0 || strcmp(a, "-interactive") == 0) interactive = 1;
		else if (strcmp(a, "-repl") == 0) { repl = 1; interactive = 1; }
		// Debugging
		else if (strcmp(a, "--debug") == 0) { nt_debug_enabled = 1; verbose = 1; }
		else if (strcmp(a, "-v") == 0 || strcmp(a, "-verbose") == 0) verbose = 1;
		else if (strcmp(a, "-vv") == 0) verbose = 2;
		else if (strcmp(a, "-vvv") == 0) verbose = 3;
		else if (strcmp(a, "-time") == 0) do_timing = 1;
		else if (strcmp(a, "-dump-ast") == 0) dump_ast = 1;
		else if (strcmp(a, "-dump-llvm") == 0) dump_llvm = 1;
		else if (strncmp(a, "--emit-ir=", 10) == 0) emit_ir_path = a + 10;
		else if (strncmp(a, "--emit-asm=", 11) == 0) emit_asm_path = a + 11;
		else if (strcmp(a, "-dump-tokens") == 0) dump_tokens = 1;
		else if (strcmp(a, "-dump-docs") == 0) dump_docs = 1;
		else if (strcmp(a, "-dump-funcs") == 0) dump_funcs = 1;
		else if (strcmp(a, "-dump-symbols") == 0) dump_symbols = 1;
		else if (strcmp(a, "-dump-stats") == 0) dump_stats = 1;
		else if (strcmp(a, "--dump-on-error") == 0) dump_on_error = 1;
		else if (strcmp(a, "-verify") == 0) verify_module = 1;
		else if (strcmp(a, "-no-std") == 0) no_std = 1;
		else if (strcmp(a, "-std") == 0) std_full = 1;
		else if (strncmp(a, "--repl-std=", 11) == 0) {
			const char *m = a + 11;
			if (strcmp(m, "none") == 0) repl_std_override = NT_STD_NONE;
			else if (strcmp(m, "prelude") == 0) repl_std_override = NT_STD_PRELUDE;
			else if (strcmp(m, "lazy") == 0) repl_std_override = NT_STD_LAZY;
			else if (strcmp(m, "full") == 0) repl_std_override = NT_STD_FULL;
			else { fprintf(stderr, "Unknown repl std mode: %s\n", m); return 1; }
		}
		else if (strcmp(a, "--plain-repl") == 0) repl_plain = 1;
		else if (strncmp(a, "--std=", 6) == 0) {
			const char *mode = a + 6;
			has_std_override = 1;
			if (strcmp(mode, "none") == 0) {
				std_override = NT_STD_NONE;
			} else if (strcmp(mode, "prelude") == 0) {
				std_override = NT_STD_PRELUDE;
			} else if (strcmp(mode, "lazy") == 0) {
				std_override = NT_STD_LAZY;
			} else if (strcmp(mode, "full") == 0) {
				std_override = NT_STD_FULL;
			} else if (strncmp(mode, "use:", 4) == 0) {
				std_override = NT_STD_USE_LIST;
				std_use_list = parse_use_list(mode + 4, &std_use_count);
			} else {
				usage(argv[0]);
			}
		}
		else if (strcmp(a, "-trace") == 0) trace_exec = 1;
		else if (strcmp(a, "-safe-mode") == 0) safe_mode = 1;
		else if (strcmp(a, "--no-strip") == 0) strip_override = 0;
		else if (strcmp(a, "--strip") == 0) strip_override = 1;
		else if (strcmp(a, "-g") == 0 || strcmp(a, "--debug-info") == 0) {
			// Allow debug symbols in emitted binary
			strip_override = 0;
		}
		else if (strncmp(a, "--color=", 8) == 0) {
			const char *mode = a + 8;
			if (strcmp(mode, "auto") == 0) nt_color_mode = -1;
			else if (strcmp(mode, "always") == 0) nt_color_mode = 1;
			else if (strcmp(mode, "never") == 0) nt_color_mode = 0;
			else usage(argv[0]);
		}
		// Info
		else if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 || strcmp(a, "--help") == 0) usage(argv[0]);
		else if (strcmp(a, "-version") == 0 || strcmp(a, "--version") == 0) {
			printf("Nytrix v0.1.0 (LLVM backend)\n");
			return 0;
		}
		// Legacy / placeholders
		else if (strcmp(a, "-g") == 0) { /* debug info placeholder */ }
		// File path
		else if (a[0] == '-') usage(argv[0]);
		else if (!file_path) {
			file_path = a;
			file_arg_idx = i;
		}
		else {
			// First non-flag after file path is considered a script arg.
			break;
		}
	}
	if (output_path) emit_only = 1;
	nt_verbose_enabled = verbose;
	if (emit_only) run_jit = 0;
	else if (file_path || inline_src || repl) run_jit = 1;
	// Default to full stdlib for JIT execution unless disabled
	if (run_jit && !no_std && !has_std_override && std_override == NT_STD_NONE) {
		std_full = 1;
	}
	// Parse args loop finished

#ifndef NYTRIX_STD_PATH
#define NYTRIX_STD_PATH "build/std_bundle.ny"
#endif
	const char *root = nytrix_src_root();
	char std_path_buf[8192];
	const char *std_path = NYTRIX_STD_PATH;
	if (root && *root) {
		snprintf(std_path_buf, sizeof(std_path_buf), "%s/build/std_bundle.ny", root);
		std_path = std_path_buf;
	}
	const char *prebuilt_path = resolve_std_bundle(std_path);
	if (prebuilt_path) {
		// Export for REPL or other components
		setenv("NYTRIX_STD_PREBUILT", prebuilt_path, 1);
	}
	if (verbose) verify_module = 1;  // Auto-enable verification in verbose mode
	// Suppress unused variable warnings for future features
	(void)trace_exec; (void)safe_mode;
	clock_t t_start = 0, t_load = 0, t_lex = 0, t_parse = 0, t_codegen = 0, t_opt = 0, t_exec = 0;
	if (do_timing) t_start = clock();
	// Interactive REPL
	if (interactive) {
		if (verbose) NT_LOG_INFO("Starting interactive REPL\n");
		const char *init_code = inline_src;
		inline_src = NULL;
		nt_repl_set_plain(repl_plain);
		if (repl_std_override != (nt_std_mode)-1) nt_repl_set_std_mode(repl_std_override);
		LLVMLinkInMCJIT();
		nt_llvm_init_native();
		LLVMLoadLibraryPermanently(NULL);
		nt_repl_run(opt_level, opt_pipeline, init_code);
		return 0;
	}
	if (verbose) {
		NT_LOG_INFO("Nytrix compiler v0.1.0\n");
		NT_LOG_INFO("LLVM backend enabled\n");
		if (opt_level > 0) NT_LOG_INFO("Optimization level: -O%d\n", opt_level);
		if (opt_pipeline) NT_LOG_INFO("Custom pipeline: %s\n", opt_pipeline);
		if (run_jit) NT_LOG_INFO("JIT execution enabled\n");
		if (emit_only) NT_LOG_INFO("Output: %s\n", output_path ? output_path : "stdout");
		if (no_std) NT_LOG_INFO("Stdlib disabled\n");
		else if (std_full) NT_LOG_INFO("Stdlib: full\n");
	}
	char *user_src = NULL;
	char *std_src = NULL;
	// 1. Read source
	if (repl) {
		if (verbose) NT_LOG_INFO("Reading from stdin...\n");
		// Read stdin
		size_t cap = 4096, len = 0;
		user_src = malloc(cap);
		if (!user_src) {
			return 1;
		}
		int ch;
		while ((ch = fgetc(stdin)) != EOF) {
			if (len + 1 >= cap) {
				cap *= 2;
				char *new_src = realloc(user_src, cap);
				if (!new_src) {
					free(user_src);
					return 1;
				}
				user_src = new_src;
			}
			user_src[len++] = (char)ch;
		}
		user_src[len] = '\0';
	} else if (inline_src) {
		if (verbose) NT_LOG_INFO("Using inline source\n");
		user_src = strdup(inline_src);
	} else if (file_path) {
		if (verbose) NT_LOG_INFO("Reading file: %s\n", file_path);
		user_src = nt_read_file(file_path);
		if (!user_src) {
			NT_LOG_ERR("Failed to read file '%s'\n", file_path);
			perror("read");
			return 1;
		}
		if (verbose) NT_LOG_INFO("User code: %zu bytes\n", strlen(user_src));
	} else {
		if (verbose) NT_LOG_INFO("No input file, using empty main\n");
		user_src = strdup("fn main(){ return 0\n }");
	}
	if (!user_src) {
		fprintf(stderr, "Failed to read source file\n");
		return 1;
	}

	// 2. Stdlib Mode Selection
	/*
	 * Logic:
	 * - Default: NT_STD_PRELUDE (if REPL) or NT_STD_NONE (if binary)?
	 *   Wait, binaries usually want std loaded unless restricted.
	 *   Actually default for files was:
	 *   - old logic: implicit load unless overridden.
	 *   - new logic:
	 *     - If `use std` is present -> FULL
	 *     - Else -> USE_LIST (only loaded what is used)
	 */
	// Re-evaluate std_mode based on flags
	nt_std_mode std_mode = std_override;
	if (!has_std_override) {
		if (no_std) std_mode = NT_STD_NONE;
		else if (std_full) std_mode = NT_STD_FULL;
		else std_mode = NT_STD_USE_LIST;
	}
	// Always collect uses to support user modules
	size_t use_count = 0;
	size_t use_cap = 0;
	char **uses = NULL;
	if (std_use_list && std_use_count) {
		// Override from flag
		uses = std_use_list;
		use_count = std_use_count;
		use_cap = use_count;
	} else {
		uses = collect_use_modules(user_src, &use_count);
		g_cleanup.user_src = user_src;
		g_cleanup.uses = uses;
		g_cleanup.use_count = use_count;
		use_cap = use_count;
	}
	if (std_mode == NT_STD_USE_LIST) {
		// If any std/lib module is used, include the prelude for core usability (e.g. print).
		if (uses && use_count) {
			for (size_t i = 0; i < use_count; ++i) {
				if (is_std_or_lib_use(uses[i])) {
					append_std_prelude(&uses, &use_count, &use_cap);
					break;
				}
			}
		}
	}
	bool use_prebuilt = false;
	/* Fast path: when the user requests the whole stdlib via `use std`,
	 * reuse the prebuilt bundle if it exists instead of rebuilding every time. */
	int wants_full_std = 0;
	if (std_mode == NT_STD_USE_LIST && uses && use_count) {
		for (size_t i = 0; i < use_count; ++i) {
			if (strcmp(uses[i], "std") == 0) { wants_full_std = 1; break; }
		}
	}
	if (!use_prebuilt && std_mode != NT_STD_NONE) {
		int prebuilt_ok = prebuilt_path && access(prebuilt_path, R_OK) == 0;
		if (prebuilt_ok) {
			struct stat st;
			if (stat(prebuilt_path, &st) == 0 && st.st_size == 0) {
				if (verbose) NT_LOG_WARN("Prebuilt std bundle is empty, ignoring\n");
				prebuilt_ok = 0;
			}
		}
		int allow_prebuilt =
			(std_mode != NT_STD_USE_LIST) ||              /* legacy path */
			(wants_full_std);                             /* `use std` */
		// Check if we have user modules that require bundling
		if (uses && use_count) {
			for (size_t i = 0; i < use_count; ++i) {
				const char *u = uses[i];
				if (strncmp(u, "std", 3) != 0 && strncmp(u, "lib", 3) != 0) {
					// User module found (e.g. gfx.mod). Must rebuild bundle to include it.
					allow_prebuilt = 0;
					break;
				}
			}
		}
		if (prebuilt_ok && allow_prebuilt) {
			use_prebuilt = true;
			if (verbose) NT_LOG_INFO("Using prebuilt std bundle: %s\n", prebuilt_path);
			std_src = nt_read_file(prebuilt_path);
			if (!std_src) {
				fprintf(stderr, "Failed to read prebuilt std bundle at %s\n", prebuilt_path);
				return 1;
			}
			/* When we shortcut a `use std` build, treat it as the full std bundle. */
			if (wants_full_std) std_mode = NT_STD_FULL;
		}
	}
	if (!use_prebuilt) {
		if (verbose) NT_LOG_INFO("Building std bundle...\n");
		std_src = nt_build_std_bundle((const char **)uses, use_count, std_mode, verbose, file_path);
		g_cleanup.std_src = std_src;
		/* Cache for future runs if we have a path - DISABLED to prevent test race conditions */
		/*
		if (std_src && prebuilt_path && *prebuilt_path && std_mode != NT_STD_USE_LIST) {
			FILE *f = fopen(prebuilt_path, "w");
			if (f) {
				fwrite(std_src, 1, strlen(std_src), f);
				fclose(f);
			} else if (verbose) {
				NT_LOG_WARN("Could not write prebuilt std bundle to %s\n", prebuilt_path);
			}
		}
		*/
	}
	if (do_timing) t_load = clock();
	/* Free collected use list regardless of the final std_mode to avoid leaks
	 * when we rewrite `use std` into NT_STD_FULL above. */
	if (uses) {
		free_use_list(uses, use_count);
		uses = NULL;
		g_cleanup.uses = NULL;
		g_cleanup.use_count = 0;
		use_count = 0;
		use_cap = 0;
	}

	// Combine sources efficiently
	size_t std_len = std_src ? strlen(std_src) : 0;
	size_t usr_len = strlen(user_src);
	char *source = malloc(std_len + usr_len + 3);  // +3 for newline separator and null terminator
	g_cleanup.source = source;
	if (!source) {
		return 1;
	}
	size_t pos = 0;
	if (std_src) {
		memcpy(source, std_src, std_len);
		pos = std_len;
		// Add newline separator between stdlib and user code
		if (std_len > 0 && source[std_len - 1] != '\n') {
			source[pos++] = '\n';
		}
	}
	memcpy(source + pos, user_src, usr_len);
	source[pos + usr_len] = '\0';
		if (getenv("NYT_DEBUG_SOURCE")) fprintf(stderr, "--- Combined Source ---\n%s\n--- End Combined Source ---\n", source);
	size_t source_len = pos + usr_len;
	// Emit cache (opt-in): reuse ELF if source+opts unchanged
	const char *cache_env = getenv("NYTRIX_CACHE");
	char cache_path[PATH_MAX] = {0};
	if (output_path && cache_env && cache_env[0] != '0' && !run_jit) {
		uint64_t h = nt_fnv1a64(source, source_len, 0);
		h = nt_fnv1a64(&opt_level, sizeof(opt_level), h);
		h = nt_fnv1a64(&std_mode, sizeof(std_mode), h);
		h = nt_fnv1a64(&strip_override, sizeof(strip_override), h);
		snprintf(cache_path, sizeof(cache_path), "build/cache/%016lx.bin", (unsigned long)h);
		nt_ensure_dir("build");
		nt_ensure_dir("build/cache");
		if (access(cache_path, R_OK) == 0) {
			if (nt_copy_file(cache_path, output_path)) {
				printf("Cached ELF: %s\n", output_path);
				return 0;
			}
		}
	}
	if (verbose) NT_LOG_INFO("Total source: %zu bytes\n", strlen(source));
	// Debug token dump
	const char *parse_name = file_path ? file_path : (inline_src ? "<inline>" : "<stdin>");
	if (std_len > 0 && usr_len > 0) parse_name = "<combined>";
	if (dump_tokens) {
		NT_LOG_INFO("Dumping tokens...\n");
		nt_lexer lx;
		nt_lexer_init(&lx, source, parse_name);
		int token_count = 0;
		for (;;) {
			nt_token t = nt_lex_next(&lx);
			printf("%d:%d kind=%d lexeme='%.*s'\n",
				   t.line, t.col, t.kind, (int)t.len, t.lexeme);
			token_count++;
			if (t.kind == NT_T_EOF) {
				NT_LOG_INFO("Total tokens: %d\n", token_count);
				return 0;
			}
		}
	}
	if (do_timing) t_lex = clock();
	// Debug: dump combined source to file
	if (getenv("NYTRIX_DUMP_SOURCE")) {
		FILE *debug_f = fopen("/tmp/nytrix_combined_source.ny", "w");
		if (debug_f) {
			fputs(source, debug_f);
			fclose(debug_f);
			if (verbose) NT_LOG_INFO("Dumped combined source to /tmp/nytrix_combined_source.ny\n");
		}
	}
	// Parse
	if (verbose) NT_LOG_INFO("Initializing parser...\n");
	if (verbose) NT_LOG_INFO("Parsing...\n");
	nt_parser parser;
	nt_parser_init(&parser, source, parse_name);
	g_cleanup.arena = parser.arena;
	if (std_len > 0 && usr_len > 0) parser.error_ctx = "in combined stdlib + user source";
	else if (std_len > 0) parser.error_ctx = "in stdlib source";
	else parser.error_ctx = "in user source";
	if (verbose) NT_LOG_INFO("Running parse_program...\n");
	static nt_program prog;
	prog = nt_parse_program(&parser);
	g_cleanup.prog = &prog;
	if (parser.had_error) {
		fprintf(stderr, "Compilation failed: %d errors encountered.\n", parser.error_count);
		if (dump_on_error) {
			nt_ensure_dir("build");
			nt_ensure_dir("build/debug");
			nt_write_text_file("build/debug/last_source.ny", source);
			fprintf(stderr, "Wrote build/debug/last_source.ny\n");
		}
#ifdef DEBUG
		fprintf(stderr, "DEBUG: Dumping tokens for context...\n");
		nt_lexer lx_debug;
		nt_lexer_init(&lx_debug, source, parse_name);
		for (int k=0; k<100; ++k) { // Dump first 100 tokens as context
			nt_token t = nt_lex_next(&lx_debug);
			if (t.kind == NT_T_EOF) break;
			fprintf(stderr, "Tok: %d '%.*s'\n", t.kind, (int)t.len, t.lexeme);
		}
#endif
		nytrix_cleanup_global();
		return 1;
	}
	if (verbose) NT_LOG_INFO("Parsed %zu top-level statements\n", prog.body.len);
	if (do_timing) t_parse = clock();
	// Dump AST
	if (dump_ast) {
		NT_LOG_INFO("AST dump:\n");
		for (size_t i = 0; i < prog.body.len; ++i) {
			nt_stmt *s = prog.body.data[i];
			printf("  [%zu] Statement kind=%d\n", i, s->kind);
			if (s->kind == NT_S_FUNC) {
				printf("      Function: %s (params=%zu)\n",
					   s->as.fn.name, s->as.fn.params.len);
			}
		}
	}
	// Dump docs
	if (dump_docs) {
		NT_LOG_INFO("Function documentation:\n");
		for (size_t i = 0; i < prog.body.len; ++i) {
			nt_stmt *s = prog.body.data[i];
			if (s->kind == NT_S_FUNC && s->as.fn.doc) {
				printf("fn %s: %s\n", s->as.fn.name, s->as.fn.doc);
			}
		}
		nytrix_cleanup_global();
		return 0;
	}
	// Code generation
	if (verbose) NT_LOG_INFO("Initializing codegen...\n");
	if (verbose) NT_LOG_INFO("Generating LLVM IR...\n");
	static nt_codegen cg;
	nt_codegen_init(&cg, &prog, "nytrix");
	g_cleanup.cg = &cg;
	g_cleanup.cg_initialized = 1;
	if (verbose) NT_LOG_INFO("Emitting IR...\n");
	nt_codegen_emit(&cg);
	LLVMValueRef script_fn = nt_codegen_emit_script(&cg, "__script_top");
	if (output_path && script_fn) {
		nytrix_ensure_aot_entry(&cg, script_fn);
	}
	if (verbose) NT_LOG_INFO("IR generation complete\n");
	if (do_timing) t_codegen = clock();
	// Optional IR/ASM emission (pre-verify)
	if (emit_ir_path) {
		if (LLVMPrintModuleToFile(cg.module, emit_ir_path, NULL) != 0) {
			NT_LOG_ERR("Failed to write IR to %s\n", emit_ir_path);
			nytrix_cleanup_global();
			return 1;
		}
		if (verbose) NT_LOG_INFO("Wrote IR to %s\n", emit_ir_path);
	}
	if (emit_asm_path) {
		if (!nt_llvm_emit_file(cg.module, emit_asm_path, LLVMAssemblyFile)) {
			NT_LOG_ERR("Failed to write ASM to %s\n", emit_asm_path);
			nytrix_cleanup_global();
			return 1;
		}
		if (verbose) NT_LOG_INFO("Wrote ASM to %s\n", emit_asm_path);
	}
	// Write IR to file
	// if (verbose) NT_LOG_INFO("Writing IR to build/out.ll\n");
	// LLVMPrintModuleToFile(cg.module, "build/out.ll", NULL);
	// Verification
	if (verify_module) {
		if (verbose) NT_LOG_INFO("Verifying LLVM module...\n");
		char *err = NULL;
		if (LLVMVerifyModule(cg.module, LLVMPrintMessageAction, &err)) {
			NT_LOG_ERR("Module verification failed:\n%s\n", err);
			LLVMDisposeMessage(err);
			nytrix_cleanup_global();
			return 1;
		}
		if (verbose) NT_LOG_INFO("Module verification passed\n");
	}
	// Optimization pipeline
	if (opt_level > 0 || opt_pipeline) {
		if (verbose) {
			if (opt_pipeline) {
				NT_LOG_INFO("Running custom pipeline: %s\n", opt_pipeline);
			} else {
				NT_LOG_INFO("Running optimization pipeline: -O%d\n", opt_level);
			}
		}
		char passes[64];
		if (!opt_pipeline) {
			snprintf(passes, sizeof(passes), "default<O%d>", opt_level);
			opt_pipeline = passes;
		}
		LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
		LLVMPassBuilderOptionsSetVerifyEach(opts, 0);
		LLVMErrorRef perr = LLVMRunPasses(cg.module, opt_pipeline, NULL, opts);
		if (perr) {
			char *msg = LLVMGetErrorMessage(perr);
			NT_LOG_ERR("Optimization pipeline failed: %s\n", msg);
			LLVMDisposeErrorMessage(msg);
		} else if (verbose) {
			NT_LOG_INFO("Optimization complete\n");
		}
		LLVMDisposePassBuilderOptions(opts);
	}
	if (do_timing) t_opt = clock();
	// Dump functions list
	if (dump_funcs) {
		NT_LOG_INFO("Compiled functions:\n");
		LLVMValueRef func = LLVMGetFirstFunction(cg.module);
		int func_count = 0;
		while (func) {
			const char *func_name = LLVMGetValueName(func);
			unsigned param_count = LLVMCountParams(func);
			fprintf(stderr, "    [%3d] %s (params=%u)\n", func_count, func_name, param_count);
			func = LLVMGetNextFunction(func);
			func_count++;
		}
		NT_LOG_INFO("Total functions: %d\n", func_count);
	}
	// Dump symbols
	if (dump_symbols) {
		NT_LOG_INFO("Runtime symbols:\n");
		const char *rt_symbols[] = {
			"rt_malloc", "rt_free", "rt_realloc", "rt_load8", "rt_store8",
			"rt_load64", "rt_store64", "rt_syscall", "rt_exit",
			"rt_dlopen", "rt_dlsym", "rt_dlclose", "rt_dlerror",
			"rt_call0", "rt_call1", "rt_call2", "rt_call3"
		};
		for (size_t i = 0; i < sizeof(rt_symbols)/sizeof(rt_symbols[0]); i++) {
			fprintf(stderr, "    %-20s registered\n", rt_symbols[i]);
		}
	}
	// Compilation statistics
	if (dump_stats) {
		NT_LOG_INFO("Compilation statistics:\n");
		fprintf(stderr, "    Source size:       %zu bytes\n", std_len + usr_len);
		if (std_len > 0) {
			fprintf(stderr, "    Stdlib size:       %zu bytes (%.1f%%)\n",
					std_len, 100.0 * std_len / (std_len + usr_len));
		}
		fprintf(stderr, "    User code size:    %zu bytes (%.1f%%)\n",
				usr_len, 100.0 * usr_len / (std_len + usr_len));
		fprintf(stderr, "    Top-level stmts:   %zu\n", prog.body.len);
		// Count different statement types
		size_t fn_count = 0, other_count = 0;
		for (size_t i = 0; i < prog.body.len; ++i) {
			if (prog.body.data[i]->kind == NT_S_FUNC) fn_count++;
			else other_count++;
		}
		fprintf(stderr, "    Functions:         %zu\n", fn_count);
		fprintf(stderr, "    Other statements:  %zu\n", other_count);
		// LLVM module stats
		LLVMValueRef func = LLVMGetFirstFunction(cg.module);
		int llvm_func_count = 0;
		while (func) {
			llvm_func_count++;
			func = LLVMGetNextFunction(func);
		}
		fprintf(stderr, "    LLVM functions:    %d\n", llvm_func_count);
		fprintf(stderr, "    Optimization:      -O%d\n", opt_level);
	}
	// Dump LLVM IR to stdout
	if (dump_llvm) {
		NT_LOG_INFO("LLVM IR:\n");
		fprintf(stderr, "================================================================================\n");
		LLVMDumpModule(cg.module);
		fprintf(stderr, "================================================================================\n");
	}
	// Write IR to file only when requested
	if (getenv("NYTRIX_EMIT_IR")) {
		nt_ensure_dir("build");
		if (verbose) NT_LOG_INFO("Writing IR to build/out.ll\n");
		LLVMPrintModuleToFile(cg.module, "build/out.ll", NULL);
	}
	char *verify_err = NULL;
	if (LLVMVerifyModule(cg.module, LLVMReturnStatusAction, &verify_err)) {
		NT_LOG_ERR("Module verification failed: %s\n", verify_err);
		LLVMDisposeMessage(verify_err);
		if (dump_on_error) {
			nt_ensure_dir("build");
			nt_ensure_dir("build/debug");
			LLVMPrintModuleToFile(cg.module, "build/debug/last_ir.ll", NULL);
			nt_write_text_file("build/debug/last_source.ny", source);
			fprintf(stderr, "Wrote build/debug/last_ir.ll\n");
		}
		// exit(1); // Don't exit yet, let's see if it crashes anyway
	} else {
		if (verbose) NT_LOG_INFO("Module verified successfully\n");
	}
	if (output_path) {
		nt_ensure_dir("build");
		char obj_path[PATH_MAX];
		char runtime_obj[PATH_MAX];
		char runtime_ast_obj[PATH_MAX];
		pid_t pid = getpid();
		snprintf(obj_path, sizeof(obj_path), "build/nytrix_emit_%d.o", (int)pid);
		snprintf(runtime_obj, sizeof(runtime_obj), "build/nytrix_runtime_%d.o", (int)pid);
		snprintf(runtime_ast_obj, sizeof(runtime_ast_obj), "build/nytrix_runtime_ast_%d.o", (int)pid);
		bool obj_created = false;
		bool runtime_created = false;
		bool runtime_ast_created = false;
		const char *extra_objs[12];
		size_t extra_count = 0;
		char dep_objs[5][PATH_MAX + 64];
		bool deps_created[5] = {false, false, false, false, false};
		size_t dep_count = 0;
		if (!nt_llvm_emit_object(cg.module, obj_path)) {
			exit_code = 1;
			goto emit_cleanup;
		}
		obj_created = true;
		const char *cc = nt_builder_choose_cc();
		bool needs_ast = LLVMGetNamedFunction(cg.module, "rt_parse_ast") != NULL;
		const char *shared_rt = NULL; // resolve_shared_runtime();
		if (shared_rt && shared_rt[0]) {
			extra_objs[extra_count++] = shared_rt;
		} else {
			if (!nt_builder_compile_runtime(cc, runtime_obj, needs_ast ? runtime_ast_obj : NULL)) {
				exit_code = 1;
				goto emit_cleanup;
			}
			runtime_created = true;
			runtime_ast_created = needs_ast;
		}
		if (needs_ast) {
			const char *const dep_sources[] = {
				"src/compiler/syntax/lexer.c",
				"src/compiler/ast/ast.c",
				"src/compiler/syntax/parser.c",
				"src/compiler/ast/ast_json.c",
				"src/compiler/runtime_state.c"
			};
			const char *root_dir = nt_src_root();
			char dep_src_path[PATH_MAX];
			static char include_arg[PATH_MAX + 12];
			snprintf(include_arg, sizeof(include_arg), "-I%s/src/include", root_dir);
			for (size_t i = 0; i < 5; ++i) {
				snprintf(dep_src_path, sizeof(dep_src_path), "%s/%s", root_dir, dep_sources[i]);
				const char *src = dep_src_path;
				const char *name = strrchr(src, '/');
				name = name ? name + 1 : src;
				snprintf(dep_objs[i], sizeof(dep_objs[i]), "build/nytrix_dep_%s_%d.o", name, (int)pid);
				const char *const dep_args[] = {
					cc, "-std=gnu11", "-Os", "-fno-pie", "-fvisibility=hidden",
					"-ffunction-sections", "-fdata-sections",
					include_arg, "-c", src, "-o", dep_objs[i], NULL
				};
				if (nt_exec_spawn(dep_args) != 0) {
					NT_LOG_ERR("Compiler dependency %s compilation failed\n", src);
					exit_code = 1;
					goto emit_cleanup;
				}
				deps_created[i] = true;
				dep_count++;
				extra_objs[extra_count++] = dep_objs[i];
			}
		}
		bool link_strip = true;
		if (strip_override == 0) link_strip = false;
		else if (strip_override == 1) link_strip = true;
		else {
			const char *strip_env = getenv("NYTRIX_STRIP_OUTPUT");
			link_strip = !strip_env || strcmp(strip_env, "0") != 0;
		}
		if (!nt_builder_link(cc, obj_path,
								 shared_rt ? NULL : runtime_obj,
								 (needs_ast && !shared_rt) ? runtime_ast_obj : NULL,
								 extra_objs, extra_count, output_path, link_strip)) {
			exit_code = 1;
			goto emit_cleanup;
		}
		if (cache_path[0]) {
			nt_copy_file(output_path, cache_path);
		}
		NT_LOG_SUCCESS("Saved ELF: %s\n", output_path);
	emit_cleanup:
		bool keep_objs = getenv("NYTRIX_KEEP_OBJECT") != NULL;
		if (obj_created && !keep_objs) unlink(obj_path);
		if (runtime_created && !keep_objs) unlink(runtime_obj);
		if (runtime_ast_created && !keep_objs) unlink(runtime_ast_obj);
		for (size_t i = 0; i < dep_count; ++i) {
			if (deps_created[i] && !keep_objs) unlink(dep_objs[i]);
		}
		if (exit_code) { nytrix_cleanup_global(); return exit_code; }
	}
	// JIT execution
	if (run_jit) {
		if (verbose) NT_LOG_INFO("Initializing JIT execution engine...\n");
		LLVMExecutionEngineRef ee;
		char *err = NULL;
		LLVMLinkInMCJIT();
		if (verbose) NT_LOG_INFO("MCJIT linked\n");
		LLVMInitializeNativeTarget();
		if (verbose) NT_LOG_INFO("Native target initialized\n");
		LLVMInitializeNativeAsmPrinter();
		if (verbose) NT_LOG_INFO("Native asm printer initialized\n");
		LLVMInitializeNativeAsmParser();
		if (verbose) NT_LOG_INFO("Native asm parser initialized\n");
		LLVMLoadLibraryPermanently(NULL);
		if (verbose) NT_LOG_INFO("Library loaded permanently\n");
		int jit_argc = argc;
		char **jit_argv = argv;
		if (file_path && file_arg_idx > 0) {
			jit_argc = argc - file_arg_idx;
			jit_argv = &argv[file_arg_idx];
		} else if (inline_src) {
			jit_argc = 1;
			static const char *dummy_argv[] = {"ny", NULL};
			jit_argv = (char**)dummy_argv;
		}
		rt_set_args(jit_argc, (int64_t)jit_argv, (int64_t)envp);
		if (verbose) NT_LOG_INFO("Args set\n");
		struct LLVMMCJITCompilerOptions options;
		LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
		options.CodeModel = LLVMCodeModelLarge;
		if (LLVMCreateMCJITCompilerForModule(&ee, cg.module, &options, sizeof(options), &err)) {
			NT_LOG_ERR("Failed to create MCJIT: %s\n", err);
			LLVMDisposeMessage(err);
			nytrix_cleanup_global();
			return 1;
		}
		cg.llvm_ctx_owned = false; // EE owns module/context now
		char *layout_str = LLVMCopyStringRepOfTargetData(LLVMGetExecutionEngineTargetData(ee));
		LLVMSetDataLayout(cg.module, layout_str);
		LLVMDisposeMessage(layout_str);
		register_jit_symbols(ee, cg.module);
		if (verbose) NT_LOG_INFO("Symbols registered via GlobalMapping\n");
		void *dm = dlsym(RTLD_DEFAULT, "rt_malloc");
		if (verbose) NT_LOG_INFO("dlsym rt_malloc: %p\n", dm);
		// Map string globals for JIT
		// We map each string array global to point to our C-allocated string data
		// This ensures the addresses baked into ptrtoint constants are correct
		for (size_t i = 0; i < cg.interns.len; i++) {
			if (cg.interns.data[i].gv) {
				LLVMAddGlobalMapping(ee, cg.interns.data[i].gv, (void *)((char *)cg.interns.data[i].data - 64));
			}
			if (cg.interns.data[i].val && cg.interns.data[i].val != cg.interns.data[i].gv) {
				// Map the pointer global to a memory location containing the pointer
				LLVMAddGlobalMapping(ee, cg.interns.data[i].val, &cg.interns.data[i].data);
			}
		}

		register_jit_symbols(ee, cg.module);
		if (verbose) {
			 NT_LOG_INFO("Listing module functions:\n");
			 LLVMValueRef func = LLVMGetFirstFunction(cg.module);
			 while (func) {
				 const char *name = LLVMGetValueName(func);
				 NT_LOG_INFO(" - %s\n", name);
				 func = LLVMGetNextFunction(func);
			 }
		}
		if (script_fn) {
			// uint64_t check_addr = LLVMGetFunctionAddress(ee, "rt_malloc");
			// if (verbose) NT_LOG_INFO("Verifying rt_malloc: 0x%lx\n", check_addr);
			uint64_t addr = LLVMGetFunctionAddress(ee, "__script_top");
			if (addr) {
				if (verbose) NT_LOG_INFO("Initializing globals (__script_top)...\n");
				((void(*)(void))addr)();
			}
		}
		LLVMValueRef main_fn_val = LLVMGetNamedFunction(cg.module, "main");
		if (main_fn_val) {
			uint64_t addr = LLVMGetFunctionAddress(ee, "main");
			if (addr) {
				int64_t (*fn)(void) = (int64_t (*)(void))addr;
				if (verbose) NT_LOG_INFO("Executing main at 0x%lx...\n", addr);
				int64_t ret = fn();
				if (ret & 1) exit_code = (int)(ret >> 1);
				else exit_code = (int)ret;
				if (verbose) NT_LOG_INFO("main returned: %ld (raw %ld)\n", (long)exit_code, ret);
			} else {
				NT_LOG_ERR("Failed to get address for main\n");
			}
		} else if (!script_fn) {
			 NT_LOG_WARN("No main() or top-level script found\n");
		}
		LLVMDisposeExecutionEngine(ee);
		cg.module = NULL;
		if (do_timing) t_exec = clock();
	} else if (verbose) {
		NT_LOG_INFO("Compilation complete (no execution)\n");
	}
	// Timing summary
	if (do_timing) {
		double total = (double)(clock() - t_start) / CLOCKS_PER_SEC;
		double load = (double)(t_load - t_start) / CLOCKS_PER_SEC;
		double parse = (double)(t_parse - t_lex) / CLOCKS_PER_SEC;
		double codegen = (double)(t_codegen - t_parse) / CLOCKS_PER_SEC;
		double opt = (double)(t_opt - t_codegen) / CLOCKS_PER_SEC;
		fprintf(stderr, "\n");
		NT_LOG_INFO("Timing summary:\n");
		fprintf(stderr, "    Load sources:  %7.4f s (%5.1f%%)\n", load, 100*load/total);
		fprintf(stderr, "    Lex/Parse:     %7.4f s (%5.1f%%)\n", parse, 100*parse/total);
		fprintf(stderr, "    Code gen:      %7.4f s (%5.1f%%)\n", codegen, 100*codegen/total);
		fprintf(stderr, "    Optimization:  %7.4f s (%5.1f%%)\n", opt, 100*opt/total);
		if (run_jit) {
			double exec = (double)(t_exec - t_opt) / CLOCKS_PER_SEC;
			fprintf(stderr, "    Execution:     %7.4f s (%5.1f%%)\n", exec, 100*exec/total);
		}
		fprintf(stderr, "    Total:         %7.4f s\n", total);
	}
	// nytrix_cleanup_global(); // Let atexit handle it
	return exit_code;
}
