#include "std_loader.h"
#include "common.h"
#include "parser.h"
#include "ast.h"

#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

typedef struct nt_std_mod {
	char *name;    // package.module
	char *path;    // file path
	char *package; // package name
} nt_std_mod;

static nt_std_mod *nt_std_mods = NULL;
static size_t nt_std_mods_len = 0;
static size_t nt_std_mods_cap = 0;

static void std_push_mod(const char *name, const char *path, const char *package) {
	if (!name || !path || !package) return;
	if (nt_std_mods_len == nt_std_mods_cap) {
		size_t nc = nt_std_mods_cap ? nt_std_mods_cap * 2 : 64;
		nt_std_mod *nm = realloc(nt_std_mods, nc * sizeof(nt_std_mod));
		if (!nm) { fprintf(stderr, "oom\n"); exit(1); }
		nt_std_mods = nm;
		nt_std_mods_cap = nc;
	}
	nt_std_mods[nt_std_mods_len++] = (nt_std_mod){
		.name = strdup(name),
		.path = strdup(path),
		.package = strdup(package),
	};
}

static int is_ny_file(const char *name) {
	size_t n = strlen(name);
	return n > 3 && strcmp(name + n - 3, ".ny") == 0;
}

static char *path_join(const char *a, const char *b) {
	size_t al = strlen(a), bl = strlen(b);
	char *out = malloc(al + bl + 2);
	if (!out) { fprintf(stderr, "oom\n"); exit(1); }
	memcpy(out, a, al);
	out[al] = '/';
	memcpy(out + al + 1, b, bl);
	out[al + 1 + bl] = '\0';
	return out;
}

static void add_module_from_path(const char *root, const char *full_path) {
	size_t rl = strlen(root);
	if (strncmp(full_path, root, rl) != 0) return;
	const char *rel = full_path + rl;
	if (*rel == '/') rel++;
	if (!is_ny_file(rel)) return;
	char *name = strdup(rel);
	if (!name) { fprintf(stderr, "oom\n"); exit(1); }
	size_t n = strlen(name);
	name[n - 3] = '\0'; // strip .ny
	n -= 3;
	// Strip .mod suffix if present (e.g. std/io/mod.ny -> std/io)
	if (n > 4 && strcmp(name + n - 4, "/mod") == 0) {
		name[n - 4] = '\0';
		n -= 4;
	}
	for (char *p = name; *p; ++p) {
		if (*p == '/') *p = '.';
	}
	// Strip "src.std." or "src.lib." prefix if present
	const char *final_name = name;
	if (strncmp(name, "src.std.", 8) == 0) {
		final_name = name + 8;
	} else if (strncmp(name, "src.lib.", 8) == 0) {
		final_name = name + 8;
	} else if (strncmp(name, "std.", 4) == 0) {
		final_name = name + 4;
	} else if (strncmp(name, "lib.", 4) == 0) {
		final_name = name + 4;
	}
	// Prefix with std./lib. based on root to keep consistent module ids.
	const char *prefix = "";
	if (strncmp(root, "src/std", 7) == 0 || strcmp(root, "std") == 0) prefix = "std.";
	else if (strncmp(root, "src/lib", 7) == 0 || strcmp(root, "lib") == 0) prefix = "lib.";
	char *final_copy = malloc(strlen(prefix) + strlen(final_name) + 1);
	if (!final_copy) { fprintf(stderr, "oom\n"); exit(1); }
	strcpy(final_copy, prefix);
	strcat(final_copy, final_name);
	const char *dot = strchr(final_copy, '.');
	char *pkg = dot ? nt_strndup(final_copy, (size_t)(dot - final_copy)) : strdup(final_copy);
	if (!pkg) { fprintf(stderr, "oom\n"); exit(1); }
	std_push_mod(final_copy, full_path, pkg);
	free(name);
	free(pkg);
}

static void scan_dir_recursive(const char *root, const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		char *fp = path_join(dir, ent->d_name);
		struct stat st;
		if (stat(fp, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				scan_dir_recursive(root, fp);
			} else if (S_ISREG(st.st_mode) && is_ny_file(fp)) {
				// NT_LOG_INFO("Found module file: %s\n", fp);
				add_module_from_path(root, fp);
			}
		}
		free(fp);
	}
	closedir(d);
}

static void nt_std_init_modules(void);

static int mod_cmp(const void *a, const void *b) {
	const nt_std_mod *ma = (const nt_std_mod *)a;
	const nt_std_mod *mb = (const nt_std_mod *)b;
	return strcmp(ma->name, mb->name);
}

static void nt_std_init_modules(void) {
	static int init = 0;
	if (init) return;
	init = 1;
	// Check for src/std first (new structure)
	struct stat st;
	if (stat("src/std", &st) == 0 && S_ISDIR(st.st_mode)) {
		scan_dir_recursive("src/std", "src/std");
		if (stat("src/lib", &st) == 0 && S_ISDIR(st.st_mode)) {
			scan_dir_recursive("src/lib", "src/lib");
		}
	} else {
		// Fallback to old structure
		scan_dir_recursive("std", "std");
		if (stat("lib", &st) == 0 && S_ISDIR(st.st_mode)) {
			scan_dir_recursive("lib", "lib");
		}
	}
	if (nt_std_mods_len > 1) {
		qsort(nt_std_mods, nt_std_mods_len, sizeof(nt_std_mod), mod_cmp);
	}
}

static const char *nt_std_prelude_list[] = {
	"std.core",
	"std.core.error",
	"std.core.reflect",
	"std.collections",
	"std.collections.dict",
	"std.collections.set",
	"std.strings.str",
	"std.iter",
	"std.io",
};

static const char *nt_std_lazy_list[] = {
	"core",
	"strings",
};

const char **nt_std_prelude(size_t *count) {
	if (count) *count = sizeof(nt_std_prelude_list) / sizeof(nt_std_prelude_list[0]);
	return nt_std_prelude_list;
}

static const char *nt_std_pkgs[64] = {0};
static size_t nt_std_pkgs_len = 0;
static int nt_std_pkgs_init = 0;

static void nt_std_init_packages(void) {
	if (nt_std_pkgs_init) return;
	nt_std_pkgs_init = 1;
	nt_std_init_modules();
	for (size_t i = 0; i < nt_std_mods_len; ++i) {
		const char *pkg = nt_std_mods[i].package;
		int seen = 0;
		for (size_t j = 0; j < nt_std_pkgs_len; ++j) {
			if (strcmp(nt_std_pkgs[j], pkg) == 0) { seen = 1; break; }
		}
		if (!seen && nt_std_pkgs_len < (sizeof(nt_std_pkgs) / sizeof(nt_std_pkgs[0]))) {
			nt_std_pkgs[nt_std_pkgs_len++] = pkg;
		}
	}
}

size_t nt_std_module_count(void) {
	nt_std_init_modules();
	return nt_std_mods_len;
}

const char *nt_std_module_name(size_t idx) {
	if (idx >= nt_std_module_count()) return NULL;
	return nt_std_mods[idx].name;
}

const char *nt_std_module_path(size_t idx) {
	if (idx >= nt_std_module_count()) return NULL;
	return nt_std_mods[idx].path;
}

size_t nt_std_package_count(void) {
	nt_std_init_packages();
	return nt_std_pkgs_len;
}

const char *nt_std_package_name(size_t idx) {
	nt_std_init_packages();
	if (idx >= nt_std_pkgs_len) return NULL;
	return nt_std_pkgs[idx];
}

static const char *strip_std_prefix(const char *name) {
	if (!name) return name;
	if (strncmp(name, "std.", 4) == 0) return name + 4;
	return name;
}

static const char *strip_pkg_prefix(const char *name) {
	if (!name) return name;
	if (strncmp(name, "std.", 4) == 0) return name + 4;
	if (strncmp(name, "lib.", 4) == 0) return name + 4;
	return name;
}

int nt_std_find_module_by_name(const char *name) {
	nt_std_init_modules();
	const char *tries[4] = {name, NULL, NULL, NULL};
	char buf_mod[256], buf_core[256];
	snprintf(buf_mod, sizeof(buf_mod), "%s.mod", name);
	snprintf(buf_core, sizeof(buf_core), "%s.core", name);
	tries[1] = buf_mod;
	tries[2] = buf_core;
	for (int t = 0; t < 3; t++) {
		const char *curr = tries[t];
		if (!curr) continue;
		// 1. Exact or suffixed
		for (size_t i = 0; i < nt_std_mods_len; ++i) {
			if (strcmp(nt_std_mods[i].name, curr) == 0) return (int)i;
		}
		// 2. With std. or lib. prefix
		if (strncmp(curr, "std.", 4) != 0 && strncmp(curr, "lib.", 4) != 0) {
			char pbuf[512];
			snprintf(pbuf, sizeof(pbuf), "std.%s", curr);
			for (size_t i = 0; i < nt_std_mods_len; ++i) {
				if (strcmp(nt_std_mods[i].name, pbuf) == 0) return (int)i;
			}
			snprintf(pbuf, sizeof(pbuf), "lib.%s", curr);
			for (size_t i = 0; i < nt_std_mods_len; ++i) {
				if (strcmp(nt_std_mods[i].name, pbuf) == 0) return (int)i;
			}
		}
	}
	return -1;
}

static int find_module_index(const char *name) {
	return nt_std_find_module_by_name(name);
}

static bool is_package_name(const char *pkg) {
	if (!pkg) return false;
	nt_std_init_modules();
	for (size_t i = 0; i < nt_std_mods_len; ++i) {
		if (strcmp(nt_std_mods[i].package, pkg) == 0) {
			return true;
		}
	}
	return false;
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz < 0) { fclose(f); return NULL; }
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t read = fread(buf, 1, (size_t)sz, f);
	buf[read] = '\0';
	fclose(f);
	return buf;
}

static void append_text(char **buf, size_t *len, size_t *cap, const char *txt) {
	if (!txt) return;
	size_t add = strlen(txt);
	if (*len + add + 2 > *cap) {
		size_t new_cap = (*len + add + 2) * 2;
		char *nb = realloc(*buf, new_cap);
		if (!nb) {
			NT_LOG_ERR("oom\n");
			exit(1);
		}
		*buf = nb;
		*cap = new_cap;
	}
	memcpy(*buf + *len, txt, add);
	*len += add;
	(*buf)[(*len)++] = '\n';
	(*buf)[*len] = '\0';
}

static void append_fn_proto(nt_stmt *s, char **hdr, size_t *len, size_t *capv) {
	if (!s) return;
	if (s->kind == NT_S_FUNC) {
		char buf[512];
		int n = snprintf(buf, sizeof(buf), "fn %s(", s->as.fn.name);
		for (size_t j = 0; j < s->as.fn.params.len; ++j) {
			const char *sep = (j + 1 < s->as.fn.params.len) ? ", " : "";
			int written = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s",
								   s->as.fn.params.data[j].name, sep);
			if (written > 0) n += written;
		}
		snprintf(buf + n, sizeof(buf) - (size_t)n, ");");
		append_text(hdr, len, capv, buf);
		return;
	}
	if (s->kind == NT_S_MODULE) {
		for (size_t i = 0; i < s->as.module.body.len; ++i) {
			append_fn_proto(s->as.module.body.data[i], hdr, len, capv);
		}
	}
}

// --- User Module Support ---

static char *find_local_module(const char *name, const char *base_dir) {
	if (!name || !*name) return NULL;
	if (!base_dir || !*base_dir) base_dir = ".";
	size_t base_len = strlen(base_dir);
	size_t name_len = strlen(name);
	size_t path_cap = base_len + 1 + name_len + 4 + 1;
	char *path = malloc(path_cap);
	if (!path) return NULL;
	strcpy(path, base_dir);
	path[base_len] = '/';
	memcpy(path + base_len + 1, name, name_len + 1);
	// Replace . with /
	for(char *p = path; *p; ++p) if(*p == '.') *p = '/';
	strcat(path, ".ny");
	if (access(path, R_OK) == 0) return path;
	// Try name/mod.ny
	size_t mod_cap = base_len + 1 + name_len + 8 + 1;
	char *path_mod = malloc(mod_cap);
	if (!path_mod) { free(path); return NULL; }
	strcpy(path_mod, base_dir);
	path_mod[base_len] = '/';
	memcpy(path_mod + base_len + 1, name, name_len + 1);
	for(char *p = path_mod; *p; ++p) if(*p == '.') *p = '/';
	strcat(path_mod, "/mod.ny");
	if (access(path_mod, R_OK) == 0) {
		free(path);
		return path_mod;
	}
	free(path);
	free(path_mod);
	return NULL;
}

static char *resolve_module_path(const char *raw, const char *base_dir, bool prefer_local, bool *is_std_out) {
	if (!raw) return NULL;
	bool explicit_std = (strncmp(raw, "std.", 4) == 0);
	bool explicit_lib = (strncmp(raw, "lib.", 4) == 0);
	bool explicit_pkg = explicit_std || explicit_lib;
	if (prefer_local && !explicit_pkg) {
		char *local = find_local_module(raw, base_dir);
		if (local) {
			if (is_std_out) *is_std_out = false;
			return local;
		}
	}
	int idx = find_module_index(raw);
	if (idx >= 0) {
		if (is_std_out) *is_std_out = true;
		return strdup(nt_std_mods[idx].path);
	}
	if (!prefer_local || explicit_pkg) return NULL;
	// Fallback to local if std/lib didn't match
	char *local = find_local_module(raw, base_dir);
	if (local) {
		if (is_std_out) *is_std_out = false;
		return local;
	}
	return NULL;
}

static char *dir_from_path(const char *path) {
	if (!path || !*path) return strdup(".");
	const char *slash = strrchr(path, '/');
	if (!slash) return strdup(".");
	size_t len = (size_t)(slash - path);
	if (len == 0) return strdup("/");
	char *out = malloc(len + 1);
	if (!out) { fprintf(stderr, "oom\n"); exit(1); }
	memcpy(out, path, len);
	out[len] = '\0';
	return out;
}

typedef struct {
	char *path;
	char *name;
	bool processed;
	bool is_std;
} mod_entry;

typedef struct {
	mod_entry *entries;
	size_t len;
	size_t cap;
} mod_list;

static int mod_entry_path_cmp(const void *a, const void *b) {
	const mod_entry *ma = (const mod_entry *)a;
	const mod_entry *mb = (const mod_entry *)b;
	if (!ma->path && !mb->path) return 0;
	if (!ma->path) return -1;
	if (!mb->path) return 1;
	return strcmp(ma->path, mb->path);
}

static void mod_list_add(mod_list *list, const char *path, const char *name, bool is_std) {
	for (size_t i = 0; i < list->len; ++i) {
		if (strcmp(list->entries[i].path, path) == 0) return;
	}
	if (list->len == list->cap) {
		size_t new_cap = list->cap ? list->cap * 2 : 16;
		list->entries = realloc(list->entries, new_cap * sizeof(mod_entry));
		list->cap = new_cap;
	}
	list->entries[list->len++] = (mod_entry){
		.path = strdup(path),
		.name = strdup(name),
		.processed = false,
		.is_std = is_std
	};
}

static void scan_dependencies(mod_list *list, size_t idx) {
	if (list->entries[idx].processed) return;
	list->entries[idx].processed = true;
	char *txt = read_file(list->entries[idx].path);
	if (!txt) return;
	nt_parser parser;
	nt_parser_init(&parser, txt, list->entries[idx].path);
	// We only parse, we don't care about errors here much, just grabbing 'use'
	// But we need a valid program structure to find 'use' statements safely
	nt_program prog = nt_parse_program(&parser);
	char *base_dir = dir_from_path(list->entries[idx].path);
	bool prefer_local = !list->entries[idx].is_std;
	for (size_t i = 0; i < prog.body.len; ++i) {
		nt_stmt *s = prog.body.data[i];
		if (s->kind != NT_S_USE) continue;
		const char *raw = s->as.use.module;
		bool explicit_std = (strcmp(raw, "std") == 0) || (strncmp(raw, "std.", 4) == 0);
		bool explicit_lib = (strcmp(raw, "lib") == 0) || (strncmp(raw, "lib.", 4) == 0);
		bool explicit_pkg = explicit_std || explicit_lib;
		bool is_std = false;
		char *path = resolve_module_path(raw, base_dir, prefer_local, &is_std);
		if (path) {
			mod_list_add(list, path, raw, is_std);
			free(path);
			continue;
		}
		// Handle std specially if we want to support 'std' package inclusion
		if (strcmp(raw, "std") == 0) {
			// If no local module matched, keep 'use std' as a no-op here
			// (stdlib prelude is handled elsewhere).
			continue;
		}
		// directory logic for std.*: if no direct module found, check if it's a package
		if ((!prefer_local || explicit_std) && (explicit_std || !explicit_pkg)) {
			// Check if it's a package wildcard or directory
			const char *pkg_name = strip_pkg_prefix(raw);
			if (is_package_name(pkg_name)) {
				// Add all modules in this package
				nt_std_init_modules();
				for (size_t k = 0; k < nt_std_mods_len; ++k) {
					if (strcmp(nt_std_mods[k].package, pkg_name) == 0) {
						mod_list_add(list, nt_std_mods[k].path, nt_std_mods[k].name, true);
					}
				}
				continue;
			}
		}
		// If we reached here, it wasn't a standard package and wasn't resolved.
		// Maybe it's a directory import for user modules? Not supported yet.
		// NT_LOG_WARN("Could not resolve module '%s'\n", raw);
	}
	nt_program_free(&prog, parser.arena);
	free(base_dir);
	free(txt);
}

char *nt_build_std_bundle(const char **modules, size_t module_count, nt_std_mode mode, int verbose, const char *entry_path) {
	if (mode == NT_STD_NONE) return NULL;
	nt_std_init_modules();
	// Fallback to prebuilt bundle if core std modules are missing (e.g. installed mode or artifacts only)
	char *prebuilt_src = NULL;
	if (nt_std_find_module_by_name("std.core.mod") < 0) {
		const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
		if (prebuilt && access(prebuilt, R_OK) == 0) {
			if (verbose) printf("Using prebuilt standard library: %s\n", prebuilt);
			prebuilt_src = read_file(prebuilt);
		}
	}
	mod_list mods = {0};
	char *entry_dir = entry_path ? dir_from_path(entry_path) : NULL;
	// 1. Seed the list
	if (mode == NT_STD_FULL) {
		for (size_t i = 0; i < nt_std_mods_len; ++i) {
			mod_list_add(&mods, nt_std_mods[i].path, nt_std_mods[i].name, true);
		}
	} else {
		// PRELUDE / LAZY / USE_LIST
		const char **seed_modules = modules;
		size_t seed_count = module_count;
		if (mode == NT_STD_PRELUDE) {
			seed_modules = nt_std_prelude_list;
			seed_count = sizeof(nt_std_prelude_list) / sizeof(nt_std_prelude_list[0]);
		} else if (mode == NT_STD_LAZY) {
			seed_modules = nt_std_lazy_list;
			seed_count = sizeof(nt_std_lazy_list) / sizeof(nt_std_lazy_list[0]);
		}
		for (size_t i = 0; i < seed_count; ++i) {
			const char *raw = seed_modules[i];
			const char *name = strip_std_prefix(raw);
			if (strcmp(name, "std") == 0) {
				// use std -> full std
				for (size_t j = 0; j < nt_std_mods_len; ++j) {
					mod_list_add(&mods, nt_std_mods[j].path, nt_std_mods[j].name, true);
				}
				continue;
			}
			// Try directory/package for std
			if (is_package_name(name)) {
				for (size_t k = 0; k < nt_std_mods_len; ++k) {
					if (strcmp(nt_std_mods[k].package, name) == 0) {
						mod_list_add(&mods, nt_std_mods[k].path, nt_std_mods[k].name, true);
					}
				}
				continue;
			}
			bool is_std = false;
			char *path = resolve_module_path(raw, entry_dir ? entry_dir : ".", true, &is_std);
			if (path) {
				mod_list_add(&mods, path, raw, is_std);
				free(path);
			} else {
				if (verbose) NT_LOG_ERR("Module not found: %s\n", raw);
				// Don't exit, might be handled otherwise or just a warning
			}
		}
	}
	// 2. Scan dependencies iteratively
	bool changed = true;
	while (changed) {
		changed = false;
		size_t current_len = mods.len;
		for (size_t i = 0; i < current_len; ++i) {
			if (!mods.entries[i].processed) {
				scan_dependencies(&mods, i);
				// scan_dependencies might add elements to mods, invalidating loop constraint?
				// We used 'current_len' so new elements are processed in next outer loop iteration or next indices
				if (mods.len > current_len) changed = true;
			}
		}
		// If we processed items without adding new ones, we might still have unprocessed items?
		// scan_dependencies sets 'processed=true'.
		// So we loop until no new items are added AND all are processed.
		// The inner loop processes [0..current_len]. If nothing added, next loop processes same range but they are already processed.
		// We need to ensure we catch newly appended items.
		// Actually, simpler:
	}
	// Re-run loop specifically to catch up with appended items
	for(size_t i=0; i<mods.len; ++i) {
		if(!mods.entries[i].processed) {
			scan_dependencies(&mods, i);
		}
	}
	// Stabilize module order to avoid dependency-order override quirks.
	if (mods.len > 1) {
		qsort(mods.entries, mods.len, sizeof(mod_entry), mod_entry_path_cmp);
	}
	// 3. Build bundle
	size_t total = 0, cap = 4096;
	if (prebuilt_src) cap += strlen(prebuilt_src);
	char *bundle = malloc(cap);
	if (!bundle) return NULL;
	bundle[0] = '\0';
	if (prebuilt_src) {
		strcpy(bundle, prebuilt_src);
		total = strlen(bundle);
		free(prebuilt_src);
	}
	// Inject prelude uses so unqualified names can resolve via use-modules.
	for (size_t i = 0; i < sizeof(nt_std_prelude_list) / sizeof(nt_std_prelude_list[0]); ++i) {
		const char *m = nt_std_prelude_list[i];
		if (m && *m) {
			char buf[256];
			snprintf(buf, sizeof(buf), "use %s;", m);
			append_text(&bundle, &total, &cap, buf);
		}
	}
	for (size_t i = 0; i < mods.len; ++i) {
		if (verbose) printf("Including module: %s (%s)\n", mods.entries[i].name, mods.entries[i].path);
		char *txt = read_file(mods.entries[i].path);
		if (txt) {
			// Check if module already has a 'module' declaration
			bool has_decl = false;
			const char *p = txt;
			while (*p) {
				while (*p && isspace(*p)) p++;
				if (!*p) break;
				if (*p == ';' || *p == '#') {
					while (*p && *p != '\n') p++;
					continue;
				}
				if (strncmp(p, "module", 6) == 0 && (isspace(p[6]) || p[6] == '\0')) {
					has_decl = true;
					break;
				}
				// If we see anything else (use, fn, etc), assume no module decl at top implies implicit wrapper needed?
				// Actually 'use' is allowed before 'module'.
				if (strncmp(p, "use", 3) == 0 && (isspace(p[3]) || p[3] == '\0')) {
					while (*p && *p != '\n') p++;
					continue;
				}
				// Any other token -> stop looking
				break;
			}
			if (has_decl) {
				append_text(&bundle, &total, &cap, txt);
				append_text(&bundle, &total, &cap, "\n");
			} else {
				// Wrap all modules in their namespace
				char *wrapped = malloc(strlen(txt) + strlen(mods.entries[i].name) + 64);
				sprintf(wrapped, "module %s {\n%s\n}", mods.entries[i].name, txt);
				append_text(&bundle, &total, &cap, wrapped);
				free(wrapped);
			}
			free(txt);
		}
	}
	// Cleanup
	for (size_t i = 0; i < mods.len; ++i) {
		free(mods.entries[i].path);
		free(mods.entries[i].name);
	}
	if (mods.entries) free(mods.entries);
	if (verbose) NT_LOG_INFO("Loaded module bundle: %zu bytes\n", total);
	free(entry_dir);
	return bundle;
}

char *nt_std_generate_header(nt_std_mode mode) {
	char *bundle = nt_build_std_bundle(NULL, 0, mode, 0, NULL);
	if (!bundle) return NULL;
	nt_parser parser;
	nt_parser_init(&parser, bundle, "<std_bundle>");
	nt_program prog = nt_parse_program(&parser);
	size_t total = 0, cap = 4096;
	char *header = malloc(cap);
	if (!header) { free(bundle); return NULL; }
	header[0] = '\0';
	for (size_t i = 0; i < prog.body.len; ++i) {
		append_fn_proto(prog.body.data[i], &header, &total, &cap);
	}
	nt_program_free(&prog, parser.arena);
	free(bundle);
	if (getenv("NYTRIX_DUMP_HEADER")) {
		FILE *df = fopen("/tmp/nytrix_std_header.ny", "w");
		if (df) { fputs(header, df); fclose(df); }
	}
	return header;
}
