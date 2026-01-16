#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

#ifdef __linux__
#include <linux/limits.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char *nt_read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz < 0) { fclose(f); return NULL; }
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(sz + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t read = fread(buf, 1, sz, f);
	buf[read] = '\0';
	fclose(f);
	return buf;
}

bool nt_copy_file(const char *src, const char *dst) {
	FILE *in = fopen(src, "rb");
	if (!in) return false;
	FILE *out = fopen(dst, "wb");
	if (!out) { fclose(in); return false; }
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return false; }
	}
	fclose(in);
	fclose(out);
	return true;
}

void nt_ensure_dir(const char *path) {
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
	}
}

void nt_write_text_file(const char *path, const char *contents) {
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		return;
	}
	fputs(contents, f);
	fclose(f);
}

uint64_t nt_fnv1a64(const void *data, size_t len, uint64_t seed) {
	const uint8_t *p = (const uint8_t *)data;
	uint64_t h = seed ? seed : 14695981039346656037ULL;
	const uint64_t prime = 1099511628211ULL;
	for (size_t i = 0; i < len; ++i) {
		h ^= p[i];
		h *= prime;
	}
	return h;
}

static bool nytrix_has_sources(const char *root) {
	char probe[8192];
	snprintf(probe, sizeof(probe), "%s/src/compiler/runtime/runtime.c", root);
	return access(probe, R_OK) == 0;
}

static char *get_executable_dir(void) {
	static char buf[PATH_MAX];
	if (buf[0]) return buf;
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len != -1) {
		buf[len] = '\0';
		char *slash = strrchr(buf, '/');
		if (slash) *slash = '\0';
		return buf;
	}
	return NULL;
}

const char *nt_src_root(void) {
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
