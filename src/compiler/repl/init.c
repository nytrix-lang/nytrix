#include "codegen.h"
#include "std_loader.h"
#include "repl_types.h"
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "repl.h"
#include "completion.h"
#include "highlight.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

const nt_doc_list *g_repl_docs = NULL;
const char *g_repl_user_code = NULL;

static char *g_repl_user_source = NULL;
static size_t g_repl_user_source_cap = 0;
static size_t g_repl_user_source_len = 0;

static char *repl_read_file(const char *path) {
	if (!path || !*path) return NULL;
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz < 0) { fclose(f); return NULL; }
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t n = fread(buf, 1, (size_t)sz, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static void doclist_free(nt_doc_list *dl) {
	if (!dl || !dl->data) return;
	for (size_t i = 0; i < dl->len; ++i) {
		free(dl->data[i].name);
		if (dl->data[i].doc) free(dl->data[i].doc);
		if (dl->data[i].def) free(dl->data[i].def);
		if (dl->data[i].src) free(dl->data[i].src);
	}
	free(dl->data);
}

static char **repl_split_lines(const char *src, size_t *out_count) {
	if (!src) { if (out_count) *out_count = 0; return NULL; }
	size_t cap = 16, count = 0;
	char **lines = malloc(cap * sizeof(char *));
	if (!lines) { if (out_count) *out_count = 0; return NULL; }
	size_t start = 0, len = strlen(src);
	for (size_t idx = 0; idx <= len; ++idx) {
		if (idx == len || src[idx] == '\n') {
			size_t n = idx - start;
			if (n > 0) {
				if (count == cap) {
					size_t new_cap = cap * 2;
					char **tmp = realloc(lines, new_cap * sizeof(char *));
					if (!tmp) break;
					lines = tmp; cap = new_cap;
				}
				char *line = malloc(n + 1);
				if (!line) break;
				memcpy(line, src + start, n); line[n] = '\0';
				lines[count++] = line;
			}
			start = idx + 1;
		}
	}
	if (out_count) *out_count = count;
	return lines;
}

static void doclist_set(nt_doc_list *dl, const char *name, const char *doc, const char *def, const char *src, int kind) {
	if (!dl || !name) return;
	for (size_t i = 0; i < dl->len; ++i) {
		if (strcmp(dl->data[i].name, name) == 0) {
			if (doc) { free(dl->data[i].doc); dl->data[i].doc = strdup(doc); }
			if (def) { free(dl->data[i].def); dl->data[i].def = strdup(def); }
			if (src) { free(dl->data[i].src); dl->data[i].src = strdup(src); }
			if (kind != 0) dl->data[i].kind = kind;
			return;
		}
	}
	if (dl->len == dl->cap) {
		size_t new_cap = dl->cap ? dl->cap * 2 : 64;
		nt_doc_entry *nd = realloc(dl->data, new_cap * sizeof(nt_doc_entry));
		if (!nd) return;
		memset(nd + dl->len, 0, (new_cap - dl->len) * sizeof(nt_doc_entry));
		dl->data = nd; dl->cap = new_cap;
	}
	dl->data[dl->len].name = strdup(name);
	dl->data[dl->len].doc = doc ? strdup(doc) : NULL;
	dl->data[dl->len].def = def ? strdup(def) : NULL;
	dl->data[dl->len].src = src ? strdup(src) : NULL;
	dl->data[dl->len].kind = kind;
	dl->len += 1;
}

static void doclist_add_recursive(nt_doc_list *dl, nt_stmt_list *body, const char *prefix) {
	for (size_t i = 0; i < body->len; ++i) {
		nt_stmt *s = body->data[i];
		if (s->kind == NT_S_FUNC) {
			char qname[512];
			const char *name = s->as.fn.name;
			if (prefix && *prefix && strncmp(name, prefix, strlen(prefix)) != 0) {
				snprintf(qname, sizeof(qname), "%s.%s", prefix, name);
				name = qname;
			}
			char def_buf[512];
			int n = snprintf(def_buf, sizeof(def_buf), "fn %s(", name);
			for (size_t j = 0; j < s->as.fn.params.len; ++j) {
				const char *sep = (j + 1 < s->as.fn.params.len) ? ", " : "";
				int written = snprintf(def_buf + n, sizeof(def_buf) - (size_t)n, "%s%s", s->as.fn.params.data[j].name, sep);
				if (written > 0) n += written;
			}
			snprintf(def_buf + n, sizeof(def_buf) - (size_t)n, ")");
			char *src = NULL;
			if (s->as.fn.src_start && s->as.fn.src_end > s->as.fn.src_start) {
				src = nt_strndup(s->as.fn.src_start, (size_t)(s->as.fn.src_end - s->as.fn.src_start));
			}
			doclist_set(dl, name, s->as.fn.doc, def_buf, src, 3); // 3 = FN
			if (src) free(src);
		} else if (s->kind == NT_S_MODULE) {
			char qname[512];
			const char *name = s->as.module.name;
			if (prefix && *prefix && strncmp(name, prefix, strlen(prefix)) != 0) {
				snprintf(qname, sizeof(qname), "%s.%s", prefix, name);
				name = qname;
			}
			char *src = NULL;
			if (s->as.module.src_start && s->as.module.src_end > s->as.module.src_start) {
				src = nt_strndup(s->as.module.src_start, (size_t)(s->as.module.src_end - s->as.module.src_start));
			}
			doclist_set(dl, name, "Module", "module", src, 2); // 2 = MOD
			if (src) free(src);
			doclist_add_recursive(dl, &s->as.module.body, name);
		}
	}
}

static void doclist_add_from_prog(nt_doc_list *dl, nt_program *prog) {
	if (!dl || !prog) return;
	doclist_add_recursive(dl, &prog->body, NULL);
}

static void repl_load_module_docs(nt_doc_list *docs, const char *name) {
	int idx = nt_std_find_module_by_name(name);
	if (idx < 0) return;
	const char *path = nt_std_module_path((size_t)idx);
	char *src = repl_read_file(path);
	if (!src) return;
	nt_parser ps; nt_parser_init(&ps, src, path);
	nt_program pr = nt_parse_program(&ps);
	if (!ps.had_error) {
		if (pr.doc) doclist_set(docs, name, pr.doc, "module", NULL, 2); // 2 = MOD
		doclist_add_recursive(docs, &pr.body, name);
	}
	nt_program_free(&pr, ps.arena); free(src);
}

static void count_unclosed_ext(const char *src, int *out_paren, int *out_brack, int *out_brace, int *out_str, int *out_comment) {
	int brace = 0, paren = 0, brack = 0, in_str = 0, in_comment = 0;
	int col = 1;
	for (size_t i = 0; src[i]; i++) {
		char c = src[i];
		if (in_comment) {
			if (c == '\n') { in_comment = 0; col = 1; }
			else col++;
			continue;
		}
		if (in_str) {
			if (c == '\\' && src[i+1]) { i++; col += 2; continue; }
			if (c == '\n') { col = 1; } else {
				if ((in_str == 1 && c == '"') || (in_str == 2 && c == '\'')) in_str = 0;
				col++;
			}
			continue;
		}
		if (c == ';') { in_comment = 1; continue; }
		if (c == '"' || c == '\'') { in_str = (c == '"') ? 1 : 2; col++; continue; }
		if (c == '{') brace++;
		else if (c == '}') brace--;
		else if (c == '(') paren++;
		else if (c == ')') paren--;
		else if (c == '[') brack++;
		else if (c == ']') brack--;
		if (c == '\n') { col = 1; } else col++;
	}
	if (out_paren) *out_paren = (paren > 0 ? paren : 0);
	if (out_brack) *out_brack = (brack > 0 ? brack : 0);
	if (out_brace) *out_brace = (brace > 0 ? brace : 0);
	if (out_str) *out_str = in_str;
	if (out_comment) *out_comment = in_comment;
}

static int is_input_complete(const char *src) {
	if (!src || !*src) return 1;
	int p = 0, bk = 0, bc = 0, s = 0, c = 0;
	count_unclosed_ext(src, &p, &bk, &bc, &s, &c);
	// We ignore 'c' (unclosed comment) because a comment implicitly closes at the end of the line (buffer).
	return (p <= 0 && bk <= 0 && bc <= 0 && s == 0);
}

static void count_unclosed(const char *src, int *out_paren, int *out_brack, int *out_brace) {
	count_unclosed_ext(src, out_paren, out_brack, out_brace, NULL, NULL);
}

static void print_incomplete_hint(const char *src) {
	int p = 0, bk = 0, bc = 0, s = 0, c = 0;
	count_unclosed_ext(src, &p, &bk, &bc, &s, &c);
	if (p <= 0 && bk <= 0 && bc <= 0 && s == 0 && c == 0) return;
	printf("%s  waiting for:", nt_clr(NT_CLR_YELLOW));
	if (p > 0) printf(" %d )", p);
	if (bk > 0) printf(" %d ]", bk);
	if (bc > 0) printf(" %d }", bc);
	if (s > 0) printf(" \"");
	if (c > 0) printf(" ;");
	printf(" (continues automatically)%s\n", nt_clr(NT_CLR_RESET));
}

static void repl_print_error_snippet(const char *src, int line, int col) {
	if (!src || line <= 0) return;
	const char *s = src; int cur = 1; const char *start = s;
	while (*s && cur < line) { if (*s == '\n') { cur++; start = s + 1; } s++; }
	const char *end = start; while (*end && *end != '\n') end++;
	fprintf(stderr, "%s|%s %.*s\n", nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET), (int)(end - start), start);
	fprintf(stderr, "%s|%s ", nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET));
	for (int i = 1; i < col; ++i) fputc(' ', stderr);
	fprintf(stderr, "%s^%s\n", nt_clr(NT_CLR_BOLD NT_CLR_RED), nt_clr(NT_CLR_RESET));
}

static char *ltrim(char *s) { if (!s) return s; while (*s && isspace((unsigned char)*s)) s++; return s; }
static void rtrim_inplace(char *s) {
	if (!s) return;
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) { s[len - 1] = '\0'; len--; }
}

static int doclist_print(const nt_doc_list *dl, const char *name) {
	if (!dl || !name || !*name) return 0;
	for (size_t i = 0; i < dl->len; ++i) {
		if (strcmp(dl->data[i].name, name) == 0) {
			const char *k_name = "Symbol";
			if (dl->data[i].kind == 1) { k_name = "Package"; }
			else if (dl->data[i].kind == 2) { k_name = "Module"; }
			else if (dl->data[i].kind == 3) { k_name = "Function"; }
			printf("%s%s %s%s%s\n", nt_clr(NT_CLR_GRAY), k_name, nt_clr(NT_CLR_BOLD), dl->data[i].name, nt_clr(NT_CLR_RESET));
			if (dl->data[i].def) printf("%s%s%s\n", nt_clr(NT_CLR_CYAN), dl->data[i].def, nt_clr(NT_CLR_RESET));
			if (dl->data[i].doc) printf("\n%s\n", dl->data[i].doc);
			if (dl->data[i].src) {
				printf("\n%sLogic:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
				const char *s = dl->data[i].src;
				while (*s && isspace(*s)) s++;
				// Highlight logic
				repl_highlight_line(s);
				printf("\n");
			}
			return 1;
		}
	}
	return 0;
}

static char *repl_assignment_target(const char *src) {
	if (!src || !*src) return NULL;
	nt_lexer lx; nt_lexer_init(&lx, src, "<repl_line>");
	nt_token t0 = nt_lex_next(&lx); if (t0.kind != NT_T_IDENT) return NULL;
	nt_token t1 = nt_lex_next(&lx); if (t1.kind != NT_T_ASSIGN) return NULL;
	nt_token t2 = nt_lex_next(&lx); if (t2.kind == NT_T_EOF || t2.kind == NT_T_SEMI) return NULL;
	return nt_strndup(t0.lexeme, t0.len);
}

static const char *repl_std_mode_name(nt_std_mode mode) {
	switch (mode) {
	case NT_STD_NONE: return "none"; case NT_STD_FULL: return "full";
	case NT_STD_PRELUDE: return "prelude"; case NT_STD_USE_LIST: return "use";
	default: return "unknown";
	}
}

static int repl_indent_next = 0;
static nt_std_mode g_repl_std_override = (nt_std_mode)-1;
static int g_repl_plain = 0;

void nt_repl_set_std_mode(nt_std_mode mode) { g_repl_std_override = mode; }
void nt_repl_set_plain(int plain) { g_repl_plain = plain; }

static int repl_pre_input_hook(void) {
	if (repl_indent_next <= 0) { rl_pre_input_hook = NULL; return 0; }
	int n = repl_indent_next; repl_indent_next = 0; rl_pre_input_hook = NULL;
	for (int i = 0; i < n; ++i) rl_insert_text(" ");
	rl_point = n; return 0;
}

static int repl_calc_indent(const char *src) {
	if (!src) return 0;
	const char *line = strrchr(src, '\n'); line = line ? line + 1 : src;
	int base = 0; while (line[base] == ' ' || line[base] == '\t') { base += (line[base] == '\t') ? 4 : 1; }
	size_t len = strlen(line); while (len > 0 && isspace((unsigned char)line[len - 1])) len--;
	int extra = 0;
	if (len > 0) {
		char last = line[len - 1];
		if (last == '{' || last == '(' || last == '[') extra = 4;
		else if (last == ':') { if (len < 2 || line[len - 2] != ':') extra = 4; }
	}
	int p = 0, b = 0, c = 0; count_unclosed(src, &p, &b, &c);
	if (p + b + c > 0 && extra == 0) extra = 4;
	return base + extra;
}

static void add_builtin_docs(nt_doc_list *docs) {
	doclist_set(docs, "rt_malloc", "Allocates n bytes of memory on the heap.", "fn rt_malloc(n)", NULL, 3);
	doclist_set(docs, "rt_free", "Frees memory previously allocated by rt_malloc.", "fn rt_free(p)", NULL, 3);
	doclist_set(docs, "rt_load8", "Loads a single byte from memory address p.", "fn rt_load8(p)", NULL, 3);
	doclist_set(docs, "rt_store8", "Stores byte v at memory address p.", "fn rt_store8(p, v)", NULL, 3);
	doclist_set(docs, "rt_load64", "Loads a 64-bit integer from memory address p.", "fn rt_load64(p)", NULL, 3);
	doclist_set(docs, "rt_store64", "Stores 64-bit integer v at memory address p.", "fn rt_store64(p, v)", NULL, 3);
	doclist_set(docs, "rt_syscall", "Executes a raw Linux system call.", "fn rt_syscall(n, a1, a2, a3, a4, a5, a6)", NULL, 3);
}

static void repl_append_user_source(const char *src) {
	size_t slen = strlen(src);
	if (!g_repl_user_source || g_repl_user_source_len + slen + 2 > g_repl_user_source_cap) {
		g_repl_user_source_cap = (g_repl_user_source_cap + slen + 256) * 2;
		g_repl_user_source = realloc(g_repl_user_source, g_repl_user_source_cap);
	}
	if (g_repl_user_source_len == 0) g_repl_user_source[0] = '\0';
	strcat(g_repl_user_source, src);
	g_repl_user_source_len += slen;
	if (g_repl_user_source_len > 0 && g_repl_user_source[g_repl_user_source_len-1] != '\n') {
		strcat(g_repl_user_source, "\n");
		g_repl_user_source_len++;
	}
}

static int is_persistent_def(const char *src) {
	if (!src) return 0;
	char *t = strdup(src);
	char *p = ltrim(t);
	int res = 0;
	if (!strncmp(p, "fn ", 3)) res = 1;
	else if (!strncmp(p, "layout ", 7)) res = 1;
	else if (!strncmp(p, "def ", 4)) res = 1;
	else if (!strncmp(p, "use ", 4)) res = 1;
	else if (!strncmp(p, "module ", 7)) res = 1;
	else {
		char *an = repl_assignment_target(src);
		if (an) { free(an); res = 1; }
	}
	free(t);
	return res;
}

static void repl_remove_def(const char *name) {
	if (!name || !g_repl_user_source) return;
	size_t name_len = strlen(name);
	size_t cap = g_repl_user_source_len + 1;
	char *out = malloc(cap);
	if (!out) return;
	out[0] = '\0';
	size_t out_len = 0;
	char *src = strdup(g_repl_user_source);
	char *line = src;
	while (line && *line) {
		char *next = strchr(line, '\n');
		if (next) *next = '\0';
		char *trim = ltrim(line);
		bool drop = false;
		if (!strncmp(trim, "def ", 4)) {
			char *p = trim + 4;
			while (*p == ' ' || *p == '\t') p++;
			if (!strncmp(p, name, name_len) && (p[name_len] == '\0' || p[name_len] == ' ' || p[name_len] == '\t' || p[name_len] == '=')) {
				drop = true;
			}
		}
		if (!drop && *line) {
			size_t line_len = strlen(line);
			if (out_len + line_len + 2 > cap) {
				cap = (cap + line_len + 256) * 2;
				out = realloc(out, cap);
				if (!out) break;
			}
			memcpy(out + out_len, line, line_len);
			out_len += line_len;
			out[out_len++] = '\n';
			out[out_len] = '\0';
		}
		if (!next) break;
		line = next + 1;
	}
	free(src);
	free(g_repl_user_source);
	g_repl_user_source = out;
	g_repl_user_source_len = out_len;
	g_repl_user_source_cap = cap;
}

void nt_repl_run(int opt_level, const char *opt_pipeline, const char *init_code) {
	(void)opt_level; (void)opt_pipeline;
	const char *plain = getenv("NYTRIX_REPL_PLAIN");
	if (g_repl_plain || (plain && plain[0] != '0') || !isatty(STDOUT_FILENO)) { nt_color_mode = 0; }
	nt_std_mode std_mode = NT_STD_FULL;
	if (g_repl_std_override != (nt_std_mode)-1) std_mode = g_repl_std_override;
	else {
		const char *env_std = getenv("NYTRIX_REPL_STD");
		if (env_std) {
			if (strcmp(env_std, "none") == 0) std_mode = NT_STD_NONE;
			else if (strcmp(env_std, "prelude") == 0) std_mode = NT_STD_PRELUDE;
			else if (strcmp(env_std, "full") == 0) std_mode = NT_STD_FULL;
		}
	}
	if (getenv("NYTRIX_REPL_NO_STD")) std_mode = NT_STD_NONE;
	nt_doc_list docs = {0}; g_repl_docs = &docs;
	add_builtin_docs(&docs);
	int repl_timing = 0;
	char **init_lines = NULL; size_t init_lines_len = 0, init_line_idx = 0;
	if (init_code && *init_code) {
		// When providing init_code (e.g. from a file or -i), process as a single unit
		// if we are NOT in interactive mode, to avoid N*N standard library overhead.
		if (!isatty(STDIN_FILENO)) {
			init_lines = malloc(sizeof(char *));
			init_lines[0] = strdup(init_code);
			init_lines_len = 1;
		} else {
			init_lines = repl_split_lines(init_code, &init_lines_len);
		}
	}
	/* LLVM initialization moved into evaluation loop for isolation. */
	char *std_src_cached = NULL;
	if (std_mode != NT_STD_NONE) {
		const char *prebuilt_env = getenv("NYTRIX_STD_PREBUILT");
		if (prebuilt_env && access(prebuilt_env, R_OK) == 0) {
			std_src_cached = repl_read_file(prebuilt_env);
		}
		if (!std_src_cached) {
			std_src_cached = nt_build_std_bundle(NULL, 0, std_mode, 0, NULL);
		}
		if (std_src_cached) {
			nt_parser parser; nt_parser_init(&parser, std_src_cached, "<repl_std>");
			nt_program prog = nt_parse_program(&parser);
			if (!parser.had_error) { doclist_add_from_prog(&docs, &prog); }
			nt_program_free(&prog, parser.arena);
		}
	}
	if (!init_code && isatty(STDOUT_FILENO)) {
		printf("%sNytrix REPL%s %s(%s)%s - Type :help for commands\n",
			   nt_clr(NT_CLR_BOLD NT_CLR_CYAN), nt_clr(NT_CLR_RESET), nt_clr(NT_CLR_GRAY),
			   repl_std_mode_name(std_mode), nt_clr(NT_CLR_RESET));
	}
	char history_path[PATH_MAX] = {0}; const char *home = getenv("HOME");
	if (home) {
		snprintf(history_path, sizeof(history_path), "%s/.nytrix_history", home);
		read_history(history_path);
		stifle_history(1000);
	}
	int tty_in = isatty(STDIN_FILENO);
	int use_readline = tty_in;
	const char *rl_env = getenv("NYTRIX_REPL_RL");
	if (rl_env && (*rl_env=='0' || strcasecmp(rl_env,"false")==0)) use_readline = 0;
	if (!use_readline && !tty_in && !init_code) { // Non-interactive stdin: slurp all input then process once.
		size_t cap = 1024, len = 0;
		char *buf = malloc(cap);
		if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
		int ch;
		while ((ch = fgetc(stdin)) != EOF) {
			if (len + 1 >= cap) {
				cap *= 2;
				char *nb = realloc(buf, cap);
				if (!nb) { free(buf); fprintf(stderr, "oom\n"); exit(1); }
				buf = nb;
			}
			buf[len++] = (char)ch;
		}
		buf[len] = '\0';
		// Batch process non-interactive stdin for performance
		init_lines = malloc(sizeof(char *));
		init_lines[0] = buf;
		init_lines_len = 1;
		init_code = "<stdin>";
	}
	if (use_readline) {
		rl_attempted_completion_function = repl_enhanced_completion;
		rl_bind_keyseq("\e[A", rl_history_search_backward); rl_bind_keyseq("\e[B", rl_history_search_forward);
		rl_bind_keyseq("\x12", rl_reverse_search_history);
	}
	char *input_buffer = NULL; int last_status = 0; int incomplete_hinted = 0;
	LLVMLinkInMCJIT();
	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();
	while (1) {
		char prompt_buf[64]; const char *prompt;
		if (input_buffer) {
			int p=0,b=0,c=0; count_unclosed(input_buffer,&p,&b,&c);
			if (p+b+c>0) {
				char w[16]=""; if(p>0)strcat(w,")"); if(b>0)strcat(w,"]"); if(c>0)strcat(w,"}");
				snprintf(prompt_buf,sizeof(prompt_buf),"%s...%s%s ",nt_clr(NT_CLR_YELLOW),w,nt_clr(NT_CLR_RESET));
			} else snprintf(prompt_buf,sizeof(prompt_buf),"%s... %s",nt_clr(NT_CLR_YELLOW),nt_clr(NT_CLR_RESET));
			prompt=prompt_buf; repl_indent_next=repl_calc_indent(input_buffer); rl_pre_input_hook=repl_pre_input_hook;
		} else {
			const char *mode_tag=""; char mode_buf[16];
			if(std_mode==NT_STD_NONE){snprintf(mode_buf,sizeof(mode_buf),"[none]");mode_tag=mode_buf;}
			else if(std_mode==NT_STD_FULL){snprintf(mode_buf,sizeof(mode_buf),"[full]");mode_tag=mode_buf;}
			const char *base=last_status?"\001\033[31m\002ny!\001\033[0m\002":"\001\033[36m\002ny\001\033[0m\002";
			if(mode_tag[0])snprintf(prompt_buf,sizeof(prompt_buf),"%s\001\033[90m\002%s\001\033[0m\002> ",base,mode_tag);
			else snprintf(prompt_buf,sizeof(prompt_buf),"%s> ",base);
			prompt=prompt_buf; rl_pre_input_hook=NULL;
		}
		char *line=NULL; int from_init=0;
		if(init_line_idx<init_lines_len){line=strdup(init_lines[init_line_idx++]);from_init=1;}
		else if(use_readline){ line=readline(prompt); }
		else {
			size_t cap = 0; ssize_t nread;
			if (tty_in) { fputs(prompt, stdout); fflush(stdout); }
			nread = getline(&line, &cap, stdin);
			if(nread == -1){ if(line) free(line); break; }
			// strip trailing newline
			if(nread>0 && line[nread-1]=='\n') line[nread-1]='\0';
		}
		if(!line)break;
		if(input_buffer && !from_init && strlen(line)==0){ free(line); if(!is_input_complete(input_buffer))continue; goto process_input; }
		if(!from_init && strlen(line)==0 && !input_buffer){ free(line); continue; }
		if(input_buffer){
			size_t len=strlen(input_buffer)+strlen(line)+2; char *nb=realloc(input_buffer,len);
			input_buffer=nb; strcat(input_buffer,"\n"); strcat(input_buffer,line); free(line);
		} else input_buffer=line;
		if(is_input_complete(input_buffer)) goto process_input;
		if(tty_in && !incomplete_hinted){ print_incomplete_hint(input_buffer); incomplete_hinted=1; }
		continue;

process_input:;
		char *full_input=input_buffer; input_buffer=NULL; incomplete_hinted=0;
		if(!full_input||!*full_input){ if(full_input)free(full_input); continue; }
		if(!from_init)add_history(full_input);
		char *trimmed=ltrim(full_input);
		if(trimmed[0]==':'){
			char *p=ltrim(trimmed+1); char *cmd=p; while(*p&&!isspace((unsigned char)*p))p++;
			if(*p){*p='\0';p++;} p=ltrim(p); rtrim_inplace(p);
			const char *cn=cmd;
			if(!strcmp(cn,"h") || !strcmp(cn,"doc")) cn="help";
			if(!strcmp(cn,"q") || !strcmp(cn,"quit")) cn="exit";
			if(!strcmp(cn,"help")){
				if(*p){
					int found = 0;
					int printed = doclist_print(&docs,p);
					if(!printed){
						if(!strcmp(p,"std")){
							printf("\n%sStandard Library Packages:%s\n", nt_clr(NT_CLR_BOLD NT_CLR_CYAN), nt_clr(NT_CLR_RESET));
							for(size_t i=0;i<nt_std_package_count();++i) {
								printf("  %-15s%s(Package)%s\n", nt_std_package_name(i), nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET));
							}
							printf("\n%sStandard Library Top-level Modules:%s\n", nt_clr(NT_CLR_BOLD NT_CLR_CYAN), nt_clr(NT_CLR_RESET));
							for(size_t i=0;i<nt_std_module_count();++i) {
								const char *m = nt_std_module_name(i);
								if (!strchr(m, '.')) printf("  %-15s%s(Module)%s\n", m, nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET));
							}
							printf("\n%sUse ':help [name]' to drill down into a package or module.%s\n", nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET));
							found=1;
						} else if(nt_std_find_module_by_name(p)>=0){
							repl_load_module_docs(&docs,p);
							printed = doclist_print(&docs,p);
						}
					}
					// If it's a module, list contents
					if(!found && nt_std_find_module_by_name(p)>=0){
						printf("\n%s'%s' contains:%s\n", nt_clr(NT_CLR_BOLD), p, nt_clr(NT_CLR_RESET));
						int count = 0;
						for(size_t i=0; i<docs.len; ++i){
							const char *name = docs.data[i].name;
							if(!strncmp(name, p, strlen(p)) && name[strlen(p)] == '.') {
								const char *sub = name + strlen(p) + 1;
								if(!strchr(sub, '.')) {
									printf("  %-25s %s%-10s%s\n", name, nt_clr(NT_CLR_GRAY),
										(docs.data[i].kind == 3 ? "Function" : (docs.data[i].kind == 2 ? "Module" : "Symbol")), nt_clr(NT_CLR_RESET));
									count++;
								}
							}
						}
						if(count == 0) printf("  (No documented members found)\n");
						found = 1;
					}
					// If it's a package, list modules
					if(!found && !printed){
						for(size_t i=0;i<nt_std_package_count();++i){
							if(!strcmp(nt_std_package_name(i),p)){
								printf("\n%sPackage '%s' Modules:%s\n", nt_clr(NT_CLR_BOLD), p, nt_clr(NT_CLR_RESET));
								for(size_t j=0;j<nt_std_module_count();++j){
									const char *m=nt_std_module_name(j);
									if(!strncmp(m,p,strlen(p))&&(m[strlen(p)]=='.'||!m[strlen(p)])) {
										const char *sub = m + strlen(p) + 1;
										if (m[strlen(p)] == '\0' || !strchr(sub, '.')) {
											printf("  %-15s %s(Module)%s\n", m, nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET));
										}
									}
								}
								found=1; break;
							}
						}
					}
					if(!found && !printed) printf("%sNo documentation found for '%s'%s\n", nt_clr(NT_CLR_RED), p, nt_clr(NT_CLR_RESET));
				} else {
					printf("%sCommands:%s\n",nt_clr(NT_CLR_BOLD NT_CLR_CYAN),nt_clr(NT_CLR_RESET));
					printf("  %-15s Exit the REPL\n", ":exit/quit");
					printf("  %-15s Reset the REPL state\n", ":reset");
					printf("  %-15s Toggle execution timing\n", ":time");
					printf("  %-15s Show persistent source (defs/vars)\n", ":vars");
					printf("  %-15s Help for [name] (e.g. :help std.io)\n", ":help [name]");
					printf("\n%sNavigation Examples:%s\n", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET));
					printf("  :help std           - List all packages\n");
					printf("  :help std.io        - List members of std.io\n");
					printf("  :help std.io.print  - Show logic of print function\n");
				}
				free(full_input); continue;
			}
			if(!strcmp(cn,"exit")){ free(full_input); break; }
			if(!strcmp(cn,"clear")){ printf("\033[2J\033[H"); free(full_input); continue; }
			if(!strcmp(cn,"reset")){
				doclist_free(&docs); memset(&docs,0,sizeof(docs));
				add_builtin_docs(&docs);
				if(std_src_cached){ free(std_src_cached); std_src_cached=NULL; }
				if(g_repl_user_source){ free(g_repl_user_source); g_repl_user_source=NULL; g_repl_user_source_len=0; g_repl_user_source_cap=0; }
				if (std_mode != NT_STD_NONE) {
					std_src_cached = nt_build_std_bundle(NULL, 0, std_mode, 0, NULL);
					if(std_src_cached){
						nt_parser ps; nt_parser_init(&ps,std_src_cached,"<std>");
						nt_program pr=nt_parse_program(&ps);
						if(!ps.had_error){ doclist_add_from_prog(&docs,&pr); }
						nt_program_free(&pr, ps.arena);
					}
				}
				printf("Reset\n"); last_status=0; free(full_input); continue;
			}
			if(!strcmp(cn,"pwd")){ char buf[PATH_MAX]; if(getcwd(buf,sizeof(buf))) printf("%s\n",buf); free(full_input); continue; }
			if(!strcmp(cn,"cd")){ if(chdir(*p?p:getenv("HOME"))!=0) perror("cd"); free(full_input); continue; }
			if(!strcmp(cn,"ls")){
				const char *path = *p ? p : ".";
				DIR *d = opendir(path);
				if(d){
					struct dirent *de;
					while((de=readdir(d))){
						if(de->d_name[0]=='.') continue;
						printf("%s  ", de->d_name);
					}
					printf("\n"); closedir(d);
				} else perror("ls");
				free(full_input); continue;
			}
			if(!strcmp(cn,"time")){ repl_timing = !repl_timing; printf("Timing: %s\n", repl_timing?"on":"off"); free(full_input); continue; }
			if(!strcmp(cn,"history")||!strcmp(cn,"hist")){
				HISTORY_STATE *st = history_get_history_state();
				for(int i=0; i<st->length; i++) if(st->entries[i]) printf("%d: %s\n", i+history_base, st->entries[i]->line);
				free(st); free(full_input); continue;
			}
			if(!strcmp(cn,"save")){
				if(!*p) printf("Usage: :save <filename>\n");
				else if(write_history(p)!=0) perror("save");
				else printf("History saved to %s\n", p);
				free(full_input); continue;
			}
			if(!strcmp(cn,"load")){
				if(!*p) printf("Usage: :load <filename>\n");
				else {
					char *src = repl_read_file(p);
					if(src){
						repl_append_user_source(src);
						printf("Loaded %s (%zu bytes)\n", p, strlen(src));
						free(src);
					} else perror("load");
				}
				free(full_input); continue;
			}
			if(!strcmp(cn,"vars")){
				if(g_repl_user_source){
					printf("%s--- Persistent Source ---%s\n%s", nt_clr(NT_CLR_BOLD), nt_clr(NT_CLR_RESET), g_repl_user_source);
				} else printf("No persistent variables defined.\n");
				free(full_input); continue;
			}
			if(!strcmp(cn,"std")){
				printf("Std mode: %s\n", repl_std_mode_name(std_mode));
				free(full_input); continue;
			}
			printf("? :%s\n",cn); last_status=1; free(full_input); continue;
		}
		if(!strcmp(trimmed,"q")||!strcmp(trimmed,"quit")||!strcmp(trimmed,"exit")){ free(full_input); break; }
		if(trimmed[0] == ';' || trimmed[0] == '\0') { free(full_input); continue; }
		int is_stmt=0; if(strchr(full_input,'{'))is_stmt=1;
		if(!strncmp(trimmed,"def ",4))is_stmt=1;
		if(!strncmp(trimmed,"fn ",3))is_stmt=1;
		if(!strncmp(trimmed,"use ",4))is_stmt=1;
		if(!strncmp(trimmed,"use ",4)){
			/* In REPL we already load the full std bundle; acknowledge and skip. */
			char *mod_name = ltrim(trimmed + 4);
			char *end = mod_name; while(*end && !isspace((unsigned char)*end) && *end != ';') end++;
			char *name = nt_strndup(mod_name, (size_t)(end - mod_name));
			repl_load_module_docs(&docs, name);
			free(name);
		}
		char *an=repl_assignment_target(full_input); if(an)is_stmt=1;
		if(!is_stmt && !strncmp(ltrim(full_input),"print",5))is_stmt=1;
		int show=(!is_stmt && std_mode!=NT_STD_NONE && tty_in);
		int show_an=(an && std_mode!=NT_STD_NONE && tty_in);
		const char *fn_name = "__repl_eval";
		size_t blen = strlen(full_input) + (an ? strlen(an) : 0) + 128;
		char *body = malloc(blen);
		if (show) snprintf(body, blen, "__repl_show(%s\n)\n", full_input);
		else if (!is_stmt) snprintf(body, blen, "return %s\n", full_input);
		else if (show_an) snprintf(body, blen, "%s\n__repl_show(%s\n)\n", full_input, an);
		else snprintf(body, blen, "%s\n", full_input);
		clock_t t0 = repl_timing ? clock() : 0;
		size_t full_slen = (std_src_cached ? strlen(std_src_cached) : 0) + (g_repl_user_source ? g_repl_user_source_len : 0) + 512 + strlen(body);
		char *full_src = malloc(full_slen);
		full_src[0] = '\0';
		if (std_src_cached) {
			strcpy(full_src, std_src_cached);
			strcat(full_src, "\n");
		}
		if (g_repl_user_source && g_repl_user_source_len > 0) {
			strcat(full_src, g_repl_user_source);
			strcat(full_src, "\n");
		}
		strcat(full_src, "fn __repl_show(x){ print(x)\n return 0 }\n");
		// We don't wrap the current body in __repl_eval yet because
		// we want nt_codegen_emit_script to handle top-level statements including our body.
		strcat(full_src, body);

		nt_parser ps_all; nt_parser_init(&ps_all, full_src, "<repl_eval_unit>");
		nt_program pr_all = nt_parse_program(&ps_all);
			if(nt_debug_enabled) fprintf(stderr, "DEBUG: Parsing complete\n");
			if(!ps_all.had_error){
				if(nt_debug_enabled) fprintf(stderr, "DEBUG: Creating LLVM context\n");
				LLVMContextRef eval_ctx = LLVMContextCreate();
				LLVMModuleRef eval_mod = LLVMModuleCreateWithNameInContext("repl_eval", eval_ctx);
				LLVMBuilderRef eval_builder = LLVMCreateBuilderInContext(eval_ctx);
				LLVMExecutionEngineRef eval_ee = NULL; char *ee_err = NULL;
				if(nt_debug_enabled) fprintf(stderr, "DEBUG: Init codegen\n");
				// Let MCJIT handle target setup
				nt_codegen cg; nt_codegen_init_with_context(&cg, &pr_all, eval_mod, eval_ctx, eval_builder);
				nt_codegen_emit(&cg);
				LLVMValueRef eval_fn = nt_codegen_emit_script(&cg, "__repl_eval");
				(void)eval_fn;
				if (cg.had_error) {
					last_status = 1;
					goto cleanup_ctx;
				}
				if (cg.had_error) {
					last_status = 1;
					goto cleanup_ctx;
				}
				char *verr = NULL;
				if (LLVMVerifyModule(eval_mod, LLVMReturnStatusAction, &verr)) {
					fprintf(stderr, "Module verification failed: %s\n", verr);
					LLVMDisposeMessage(verr);
					goto cleanup_ctx;
				}
				if (verr) LLVMDisposeMessage(verr); // Just in case

				struct LLVMMCJITCompilerOptions options;
				LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
				options.CodeModel = LLVMCodeModelLarge;
				if (LLVMCreateMCJITCompilerForModule(&eval_ee, eval_mod, &options, sizeof(options), &ee_err) != 0) {
					fprintf(stderr, "Failed to create execution engine: %s\n", ee_err);
					LLVMDisposeMessage(ee_err);
					LLVMContextDispose(eval_ctx);
					continue;
				}
				// Register interned strings
				for (size_t i = 0; i < cg.interns.len; ++i) {
					if (cg.interns.data[i].gv) {
						// const_string_ptr stores data at base+64, so map global to base
						LLVMAddGlobalMapping(eval_ee, cg.interns.data[i].gv, (void *)((char *)cg.interns.data[i].data - 64));
					}
				}
				extern int64_t rt_malloc(int64_t); extern int64_t rt_init_str(int64_t, int64_t); extern int64_t rt_free(int64_t); extern int64_t rt_realloc(int64_t, int64_t);
				extern int64_t rt_load8(int64_t); extern int64_t rt_store8(int64_t, int64_t);
				extern int64_t rt_load16(int64_t); extern int64_t rt_store16(int64_t, int64_t);
				extern int64_t rt_load32(int64_t); extern int64_t rt_store32(int64_t, int64_t);
				extern int64_t rt_load64(int64_t); extern int64_t rt_store64(int64_t, int64_t);
				extern int64_t rt_load8_idx(int64_t, int64_t); extern int64_t rt_store8_idx(int64_t, int64_t, int64_t);
				extern int64_t rt_load16_idx(int64_t, int64_t); extern int64_t rt_store16_idx(int64_t, int64_t, int64_t);
				extern int64_t rt_load32_idx(int64_t, int64_t); extern int64_t rt_store32_idx(int64_t, int64_t, int64_t);
				extern int64_t rt_load64_idx(int64_t, int64_t); extern int64_t rt_store64_idx(int64_t, int64_t, int64_t);
				extern int64_t rt_sys_read_off(int64_t, int64_t, int64_t, int64_t); extern int64_t rt_sys_write_off(int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_ptr_add(int64_t, int64_t); extern int64_t rt_ptr_sub(int64_t, int64_t);
				extern int64_t rt_syscall(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_execve(int64_t, int64_t, int64_t);
				extern int64_t rt_exit(int64_t); extern int64_t rt_panic(int64_t);
				extern int64_t rt_add(int64_t, int64_t); extern int64_t rt_sub(int64_t, int64_t);
				extern int64_t rt_mul(int64_t, int64_t); extern int64_t rt_div(int64_t, int64_t); extern int64_t rt_mod(int64_t, int64_t);
				extern int64_t rt_and(int64_t, int64_t); extern int64_t rt_or(int64_t, int64_t); extern int64_t rt_xor(int64_t, int64_t);
				extern int64_t rt_shl(int64_t, int64_t); extern int64_t rt_shr(int64_t, int64_t); extern int64_t rt_not(int64_t);
				extern int64_t rt_is_int(int64_t); extern int64_t rt_is_ptr(int64_t);
				extern int64_t rt_to_int(int64_t); extern int64_t rt_from_int(int64_t);
				extern int64_t rt_str_concat(int64_t, int64_t); extern int64_t rt_eq(int64_t, int64_t);
				extern int64_t rt_to_str(int64_t); extern int64_t rt_set_globals(int64_t);
				extern int64_t rt_lt(int64_t, int64_t); extern int64_t rt_le(int64_t, int64_t); extern int64_t rt_gt(int64_t, int64_t); extern int64_t rt_ge(int64_t, int64_t);
				extern int64_t rt_flt_box_val(int64_t); extern int64_t rt_flt_unbox_val(int64_t);
				extern int64_t rt_flt_add(int64_t, int64_t); extern int64_t rt_flt_sub(int64_t, int64_t);
				extern int64_t rt_flt_mul(int64_t, int64_t); extern int64_t rt_flt_div(int64_t, int64_t);
				extern int64_t rt_flt_lt(int64_t, int64_t); extern int64_t rt_flt_gt(int64_t, int64_t); extern int64_t rt_flt_eq(int64_t, int64_t);
				extern int64_t rt_flt_from_int(int64_t); extern int64_t rt_flt_to_int(int64_t); extern int64_t rt_flt_trunc(int64_t);
				extern int64_t rt_thread_spawn(int64_t, int64_t); extern int64_t rt_thread_join(int64_t);
				extern int64_t rt_mutex_new(void); extern int64_t rt_mutex_lock64(int64_t); extern int64_t rt_mutex_unlock64(int64_t); extern int64_t rt_mutex_free(int64_t);
				extern int64_t rt_dlopen(int64_t, int64_t); extern int64_t rt_dlsym(int64_t, int64_t); extern int64_t rt_dlclose(int64_t); extern char *rt_dlerror(void);
				extern int64_t rt_call0(int64_t); extern int64_t rt_call1(int64_t, int64_t); extern int64_t rt_call2(int64_t, int64_t, int64_t); extern int64_t rt_call3(int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call4(int64_t, int64_t, int64_t, int64_t, int64_t); extern int64_t rt_call5(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call6(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t); extern int64_t rt_call7(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call8(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call9(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call10(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call11(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call12(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_call13(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_argc(void); extern int64_t rt_argv(int64_t); extern int64_t rt_envp(void); extern int64_t rt_envc(void); extern int64_t rt_errno(void);
				extern int64_t rt_set_panic_env(int64_t); extern int64_t rt_clear_panic_env(void);
				extern int64_t rt_jmpbuf_size(void); extern int64_t rt_get_panic_val(void);
				extern int64_t rt_globals(void);
				extern int64_t rt_recv(int64_t, int64_t, int64_t, int64_t);
				extern int64_t rt_kwarg(int64_t, int64_t);
				extern int64_t rt_sleep(int64_t);
				extern int64_t rt_set_args(int64_t, int64_t, int64_t);
				extern int64_t rt_parse_ast(int64_t);
				struct { const char *n; void *p; } syms[] = {
					{"rt_malloc", (void*)rt_malloc}, {"rt_init_str", (void*)rt_init_str}, {"rt_free", (void*)rt_free}, {"rt_realloc", (void*)rt_realloc},
					{"rt_load8", (void*)rt_load8}, {"rt_store8", (void*)rt_store8}, {"rt_load16", (void*)rt_load16}, {"rt_store16", (void*)rt_store16},
					{"rt_load32", (void*)rt_load32}, {"rt_store32", (void*)rt_store32}, {"rt_load64", (void*)rt_load64}, {"rt_store64", (void*)rt_store64},
					{"rt_load8_idx", (void*)rt_load8_idx}, {"rt_store8_idx", (void*)rt_store8_idx},
					{"rt_load16_idx", (void*)rt_load16_idx}, {"rt_store16_idx", (void*)rt_store16_idx},
					{"rt_load32_idx", (void*)rt_load32_idx}, {"rt_store32_idx", (void*)rt_store32_idx},
					{"rt_load64_idx", (void*)rt_load64_idx}, {"rt_store64_idx", (void*)rt_store64_idx},
					{"rt_ptr_add", (void*)rt_ptr_add}, {"rt_ptr_sub", (void*)rt_ptr_sub},
					{"rt_sys_read_off", (void*)rt_sys_read_off}, {"rt_sys_write_off", (void*)rt_sys_write_off},
					{"rt_syscall", (void*)rt_syscall}, {"rt_execve", (void*)rt_execve}, {"rt_exit", (void*)rt_exit}, {"rt_panic", (void*)rt_panic},
					{"rt_add", (void*)rt_add}, {"rt_sub", (void*)rt_sub}, {"rt_mul", (void*)rt_mul}, {"rt_div", (void*)rt_div}, {"rt_mod", (void*)rt_mod},
					{"rt_and", (void*)rt_and}, {"rt_or", (void*)rt_or}, {"rt_xor", (void*)rt_xor}, {"rt_shl", (void*)rt_shl}, {"rt_shr", (void*)rt_shr}, {"rt_not", (void*)rt_not},
					{"rt_str_concat", (void*)rt_str_concat}, {"rt_eq", (void*)rt_eq},
					{"rt_lt", (void*)rt_lt}, {"rt_le", (void*)rt_le}, {"rt_gt", (void*)rt_gt}, {"rt_ge", (void*)rt_ge},
					{"rt_to_str", (void*)rt_to_str}, {"rt_set_globals", (void*)rt_set_globals},
					{"rt_is_int", (void*)rt_is_int}, {"rt_is_ptr", (void*)rt_is_ptr}, {"rt_to_int", (void*)rt_to_int}, {"rt_from_int", (void*)rt_from_int},
					{"rt_flt_box_val", (void*)rt_flt_box_val}, {"rt_flt_unbox_val", (void*)rt_flt_unbox_val},
					{"rt_flt_add", (void*)rt_flt_add}, {"rt_flt_sub", (void*)rt_flt_sub}, {"rt_flt_mul", (void*)rt_flt_mul}, {"rt_flt_div", (void*)rt_flt_div},
					{"rt_flt_lt", (void*)rt_flt_lt}, {"rt_flt_gt", (void*)rt_flt_gt}, {"rt_flt_eq", (void*)rt_flt_eq},
					{"rt_flt_from_int", (void*)rt_flt_from_int}, {"rt_flt_to_int", (void*)rt_flt_to_int}, {"rt_flt_trunc", (void*)rt_flt_trunc},
					{"rt_thread_spawn", (void*)rt_thread_spawn}, {"rt_thread_join", (void*)rt_thread_join},
					{"rt_mutex_new", (void*)rt_mutex_new}, {"rt_mutex_lock64", (void*)rt_mutex_lock64},
					{"rt_mutex_unlock64", (void*)rt_mutex_unlock64}, {"rt_mutex_free", (void*)rt_mutex_free},
					{"rt_dlopen", (void*)rt_dlopen}, {"rt_dlsym", (void*)rt_dlsym}, {"rt_dlclose", (void*)rt_dlclose}, {"rt_dlerror", (void*)rt_dlerror},
					{"rt_call0", (void*)rt_call0}, {"rt_call1", (void*)rt_call1}, {"rt_call2", (void*)rt_call2}, {"rt_call3", (void*)rt_call3},
					{"rt_call4", (void*)rt_call4}, {"rt_call5", (void*)rt_call5}, {"rt_call6", (void*)rt_call6}, {"rt_call7", (void*)rt_call7},
					{"rt_call8", (void*)rt_call8}, {"rt_call9", (void*)rt_call9}, {"rt_call10", (void*)rt_call10}, {"rt_call11", (void*)rt_call11},
					{"rt_call12", (void*)rt_call12}, {"rt_call13", (void*)rt_call13},
					{"rt_argc", (void*)rt_argc}, {"rt_argv", (void*)rt_argv}, {"rt_envp", (void*)rt_envp}, {"rt_envc", (void*)rt_envc}, {"rt_errno", (void*)rt_errno},
					{"rt_set_panic_env", (void*)rt_set_panic_env}, {"rt_clear_panic_env", (void*)rt_clear_panic_env},
					{"rt_jmpbuf_size", (void*)rt_jmpbuf_size}, {"rt_get_panic_val", (void*)rt_get_panic_val},
					{"rt_globals", (void*)rt_globals}, {"rt_recv", (void*)rt_recv}, {"rt_kwarg", (void*)rt_kwarg},
					{"rt_sleep", (void*)rt_sleep}, {"rt_set_args", (void*)rt_set_args}, {"rt_parse_ast", (void*)rt_parse_ast},
					{NULL, NULL}
				};
				// Mapping runtime symbols (handled suffixes like .9)
				for (LLVMValueRef f = LLVMGetFirstFunction(eval_mod); f; f = LLVMGetNextFunction(f)) {
					const char *fn_name_llvm = LLVMGetValueName(f);
					for (int i = 0; syms[i].n; ++i) {
						size_t slen = strlen(syms[i].n);
						if (strncmp(fn_name_llvm, syms[i].n, slen) == 0 && (fn_name_llvm[slen] == '\0' || fn_name_llvm[slen] == '.')) {
							LLVMAddGlobalMapping(eval_ee, f, syms[i].p);
							break;
						}
					}
				}
				if(nt_debug_enabled) fprintf(stderr, "DEBUG: Interpreter execution start\n");
				LLVMValueRef eval_fn_val = LLVMGetNamedFunction(eval_mod, fn_name);
				if (eval_fn_val) {
					LLVMGenericValueRef res_gv = LLVMRunFunction(eval_ee, eval_fn_val, 0, NULL);
					LLVMDisposeGenericValue(res_gv);
					// if(!is_stmt && !show) printf("%s=>%s %ld\n", nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET), res_val);
					last_status = 0;
					char *undef_name = NULL;
					if (!strncmp(ltrim(full_input), "undef ", 6)) {
						char *p = ltrim(full_input) + 6;
						while (*p == ' ' || *p == '\t') p++;
						char *end = p;
						while (*end && !isspace((unsigned char)*end) && *end != ';') end++;
						if (end > p) undef_name = nt_strndup(p, (size_t)(end - p));
					}
					if (undef_name) {
						repl_remove_def(undef_name);
						free(undef_name);
					} else if (is_persistent_def(full_input)) {
						repl_append_user_source(full_input);
					}
				} else {
					fprintf(stderr, "Failed to find function %s\n", fn_name);
					last_status = 1;
				}
			clock_t t_end = clock();
			if (repl_timing && !from_init) {
				printf("%s[Eval: %.3f ms]%s\n", nt_clr(NT_CLR_GRAY), (double)(t_end - t0) * 1000.0 / CLOCKS_PER_SEC, nt_clr(NT_CLR_RESET));
			}
			LLVMDisposeExecutionEngine(eval_ee);
			cleanup_ctx:
			nt_codegen_dispose(&cg);
			LLVMContextDispose(eval_ctx);
		} else if(ps_all.error_count > 0){
			repl_print_error_snippet(full_src, ps_all.cur.line, ps_all.cur.col);
			last_status = 1;
		}
	nt_program_free(&pr_all, ps_all.arena); free(full_src); free(body); if(an) free(an); free(full_input);
	}
	if (history_path[0]) write_history(history_path);
	doclist_free(&docs); if(std_src_cached) free(std_src_cached);
	if(init_lines){ for(size_t i=0;i<init_lines_len;++i)free(init_lines[i]); free(init_lines); }
}
