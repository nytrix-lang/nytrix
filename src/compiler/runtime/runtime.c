/*
 * Nytrix Runtime
 * Philosophy: explicit, zero-cost, minimal abstractions
 *
 * A bit bloated, Still not there but getting there...
 *
 * Features:
 * - core functions (malloc/free, load/store, syscall, FFI, exit)
 * - Pooled allocation for small objects (8-256B)
 * - mmap for large blocks (>4KB, no heap fragmentation)
 * - Inline syscalls on x86_64 (zero libc overhead)
 * - 8-byte aligned operations (8x faster than byte-wise)
 * - Debug statistics (NYTRIX_MEM_STATS=1)
 *
 * All stdlib implemented in .ny files - this is just the kernel!
 */

// TODO: try to do more syscalls in std to remove more runtime rt_ functions

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <time.h>
#include <dlfcn.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <pthread.h>
#include "common.h"

#include <errno.h>

#define NT_MAGIC1 0x545249584E5954ULL
#define NT_MAGIC2 0x4E59545249584EULL
#define NT_MAGIC_END 0xDEADBEEFCAFEBABEULL
static inline int is_int(int64_t v) { return (v & 1); }
#define is_ptr(v) ((v) != 0 && ((v) & 7) == 0 && (uintptr_t)(v) > 0x1000)
#define is_heap_ptr(v) ((v) != 0 && ((v) & 63) == 0 && (uintptr_t)(v) > 0x1000 && (*(uint64_t*)((uintptr_t)(v) - 64) == NT_MAGIC1) && (*(uint64_t*)((uintptr_t)(v) - 48) == NT_MAGIC2))
#define is_any_ptr(v) (((v) != 0 && !((v) & 1) && (uintptr_t)(v) > 0x1000))
static inline int64_t rt_tag(int64_t v) { return (v << 1) | 1; }
static inline int64_t rt_untag(int64_t v) { return (v & 1) ? (v >> 1) : v; }
static inline int64_t rt_mask_ptr(int64_t v) { return (int64_t)(v & ~7ULL); }

#define TAG_FLOAT     221 // (110 << 1) | 1
#define TAG_STR       241 // (120 << 1) | 1
#define TAG_STR_CONST 243 // (121 << 1) | 1

static inline int is_v_flt(int64_t v) {
	if (!is_ptr(v)) return 0;
	int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
	return (tag == TAG_FLOAT || tag == 110);
}
static inline int is_ny_obj(int64_t v) {
	if (!is_ptr(v)) return 0;
	int64_t tag = *(int64_t*)((uintptr_t)v - 8);
	return (tag >= 100 && tag <= 119) || (tag >= 200 && tag <= 250);
}
static inline int is_v_str(int64_t v) {
	if (!is_ptr(v)) return 0;
	int64_t tag = *(int64_t*)((char *)(uintptr_t)v - 8);
	return (tag == TAG_STR || tag == TAG_STR_CONST || tag == 120 || tag == 121);
}

int64_t rt_malloc(int64_t n);
int64_t rt_is_int(int64_t v) { return is_int(v) ? 2 : 4; }
int64_t rt_is_ptr(int64_t v) { return is_ptr(v) ? 2 : 4; }
int64_t rt_is_flt(int64_t v) { return is_v_flt(v) ? 2 : 4; }
int64_t rt_is_str(int64_t v) { return is_v_str(v) ? 2 : 4; }

static uint64_t _rt_rng_state = 0x123456789ABCDEF0ULL;
static int _rt_rng_forced_prng = 0;
int64_t rt_srand(int64_t s);
int64_t rt_rand64(void);

int64_t rt_flt_add(int64_t a, int64_t b);
int64_t rt_flt_sub(int64_t a, int64_t b);
int64_t rt_flt_mul(int64_t a, int64_t b);
int64_t rt_flt_div(int64_t a, int64_t b);
int64_t rt_flt_lt(int64_t a, int64_t b);
int64_t rt_flt_le(int64_t a, int64_t b);
int64_t rt_flt_gt(int64_t a, int64_t b);
int64_t rt_flt_ge(int64_t a, int64_t b);
int64_t rt_flt_eq(int64_t a, int64_t b);
int64_t rt_flt_to_int(int64_t v);
int64_t rt_flt_box_val(int64_t bits);

int64_t rt_flt_unbox_val(int64_t v) {
	if (v & 1) { // is_int
		double d = (double)(v >> 1);
		int64_t res;
		memcpy(&res, &d, 8);
		return res;
	}
	// Check if it's a boxed float
	if (is_ptr(v)) {
		int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
		if (tag == TAG_FLOAT) {
			int64_t bits;
			memcpy(&bits, (void *)(uintptr_t)v, 8);
			return bits;
		}
	}
	return 0;
}

int64_t rt_srand(int64_t s) {
	_rt_rng_state = (uint64_t)(s >> 1);
	_rt_rng_forced_prng = 1;
	return s;
}

int64_t rt_rand64(void) {
	uint64_t val;
	int ok = 0;

#if defined(__x86_64__)
	if (!_rt_rng_forced_prng) {
		__asm__ volatile("rdrand %0; setc %b1" : "=r"(val), "=q"(ok));
	}
#endif
	if (!ok) {
		// Fallback: SplitMix64 or similar
		_rt_rng_state += 0x9e3779b97f4a7c15ULL;
		uint64_t z = _rt_rng_state;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		val = z ^ (z >> 31);
	}
	uint64_t res = ((uint64_t)(val & 0x3FFFFFFFFFFFFFFFULL) << 1) | 1ULL;
	return (int64_t)res;
}

int64_t rt_flt_box_val(int64_t bits) {
	int64_t res = rt_malloc(17); // (8 << 1) | 1
	*(int64_t *)((char *)(uintptr_t)res - 8) = TAG_FLOAT;
	memcpy((void *)(uintptr_t)res, &bits, 8);
	return res;
}

// Memory Management
static inline size_t rt_get_heap_size(int64_t v) {
	if (!is_heap_ptr(v)) return (size_t)-1;
	return *(uint64_t*)((uintptr_t)v - 56);
}

static inline int rt_check_oob(const char *op, int64_t addr, int64_t idx, size_t access_sz) {
	(void)op;
	if (!is_heap_ptr(addr)) return 1;
	size_t sz = rt_get_heap_size(addr);
	// Handle negative indices (Header access) separately
	if ((intptr_t)idx < 0) {
		// Header is 64 bytes
		if ((intptr_t)idx < -64) return 0;
		return 1;
	}
	// Normal body access
	if ((size_t)idx + access_sz > sz) {
		return 0;
	}
	return 1;
}

#ifndef NDEBUG
static uint64_t g_alloc, g_free, g_pool_hits;
#endif

static int g_argc = 0;
static int g_envc = 0;
static char **g_argv = NULL;
static char **g_envp = NULL;

#include <setjmp.h>
#include <inttypes.h>
#include <inttypes.h>

static int64_t g_globals_ptr = 0;
typedef NT_VEC(jmp_buf*) jmp_buf_vec;
static jmp_buf_vec g_panic_env_stack = {0};
static int64_t g_panic_value = 0;

int64_t rt_errno(void) {
	return errno;
}

int64_t rt_globals(void) { return g_globals_ptr; }

int64_t rt_set_globals(int64_t p) { g_globals_ptr = p; return p; }

int64_t rt_set_panic_env(int64_t env_ptr) {
	nt_vec_push(&g_panic_env_stack, (jmp_buf *)(uintptr_t)env_ptr);
	return 0;
}
int64_t rt_clear_panic_env(void) {
	if (g_panic_env_stack.len > 0) {
		g_panic_env_stack.len--;
	}
	return 0;
}
int64_t rt_jmpbuf_size(void) { return (int64_t)sizeof(jmp_buf); }
int64_t rt_get_panic_val(void) { return g_panic_value; }

int64_t rt_panic(int64_t msg_ptr) {
	if (g_panic_env_stack.len > 0) {
		g_panic_value = msg_ptr;
		longjmp(*(g_panic_env_stack.data[g_panic_env_stack.len - 1]), 1);
	}
	fprintf(stderr, "Panic: %s\n", (char *)(uintptr_t)msg_ptr);
	exit(1);
	return 0;
}

int64_t rt_malloc(int64_t size) {
	if (is_int(size)) size >>= 1;
	if (size < 0) return 0;
	if (size < 64) size = 64;
	size = (size + 63) & ~63ULL;
	size_t total = (size_t)size + 128;
	void *p = NULL;
	// Debug print for rt_malloc
	// fprintf(stderr, "[rt_malloc] Requesting %lu bytes (raw size %lu)\n", total, size);
	if (posix_memalign(&p, 64, total) != 0) return 0;
	memset(p, 0, total);
	int64_t res = (int64_t)(uintptr_t)((char*)p + 64);
	*(uint64_t *)p = NT_MAGIC1;
	*(uint64_t *)((char *)p + 8) = (uint64_t)size;
	*(uint64_t *)((char *)p + 16) = NT_MAGIC2;
	*(uint64_t *)((char *)p + 64 + size) = NT_MAGIC_END;
	return res;
}

int64_t rt_sleep(int64_t ms) {
	if (is_int(ms)) ms >>= 1;
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;
	nanosleep(&ts, NULL);
	return 1;
}

int64_t rt_free(int64_t ptr) {
	if (is_heap_ptr(ptr)) {
		size_t sz = rt_get_heap_size(ptr);
		if (*(uint64_t*)((char*)(uintptr_t)ptr + sz) != NT_MAGIC_END) {
			 // fprintf(stderr, "[MEM] Overflow detected at 0x%lx\n", (unsigned long)ptr);
		}
		free((char *)(uintptr_t)ptr - 64);
	}
	return 0;
}
int64_t rt_realloc(int64_t p_val, int64_t newsz) {
	if (is_int(newsz)) newsz >>= 1;
	if (newsz < 0) newsz = 0;
	if (!is_heap_ptr(p_val)) return rt_malloc(newsz << 1 | 1);
	char *op = (char *)(uintptr_t)p_val - 64;
	size_t old_size = *(uint64_t *)(op + 8);
	if ((size_t)newsz <= old_size) {
		 return p_val;
	}
	int64_t res = rt_malloc(newsz << 1 | 1);
	if (!res) return 0;
	memcpy((void*)(uintptr_t)res, (char*)p_val, old_size);
	rt_free(p_val);
	return res;
}

int64_t rt_memcpy(int64_t dst, int64_t src, int64_t n) {
	if (is_int(n)) n >>= 1;
	if (n <= 0) return dst;
	if (!rt_check_oob("memcpy_dst", dst, 0, (size_t)n)) return dst;
	if (!rt_check_oob("memcpy_src", src, 0, (size_t)n)) return dst;
	memcpy((void *)(uintptr_t)dst, (void *)(uintptr_t)src, (size_t)n);
	return dst;
}

int64_t rt_memset(int64_t dst, int64_t v, int64_t n) {
	if (is_int(v)) v >>= 1;
	if (is_int(n)) n >>= 1;
	if (n > 0) memset((void *)(uintptr_t)dst, (int)v, (size_t)n);
	return dst;
}

int64_t rt_memcmp(int64_t a, int64_t b, int64_t n) {
	if (is_int(n)) n >>= 1;
	if (n <= 0) return 1; // tagged 0
	int res = memcmp((void *)(uintptr_t)a, (void *)(uintptr_t)b, (size_t)n);
	return (int64_t)(res << 1) | 1;
}

int64_t rt_load8_idx(int64_t addr, int64_t idx) {
	if (!is_any_ptr(addr)) return 1;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("load8", addr, idx, 1)) {
		return 1;
	}
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return 1;
	int64_t val = (((int64_t)*(uint8_t *)p) << 1) | 1;
	if (val == 1) {
	}
	return val;
}
int64_t rt_load16_idx(int64_t addr, int64_t idx) {
	if (!is_any_ptr(addr)) return 1;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("load16", addr, idx, 2)) return 1;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return 1;
	return (((int64_t)*(uint16_t *)p) << 1) | 1;
}
int64_t rt_load32_idx(int64_t addr, int64_t idx) {
	if (!is_any_ptr(addr)) return 1;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("load32", addr, idx, 4)) return 1;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return 1;
	return (((int64_t)*(uint32_t *)p) << 1) | 1;
}
int64_t rt_load64_idx(int64_t addr, int64_t idx) {
	if (!is_any_ptr(addr)) return 0;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("load64", addr, idx, 8)) return 0;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return 0;
	return *(int64_t *)p;
}
int64_t rt_store8_idx(int64_t addr, int64_t idx, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("store8", addr, idx, 1)) return val;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) {
		return val;
	}
	int64_t v = (val & 1) ? (val >> 1) : val;
	*(uint8_t *)p = (uint8_t)v;
	if (*(uint8_t*)p != (uint8_t)v) {
	}
	return val;
}
int64_t rt_store16_idx(int64_t addr, int64_t idx, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("store16", addr, idx, 2)) return val;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return val;
	int64_t v = (val & 1) ? (val >> 1) : val;
	*(uint16_t *)p = (uint16_t)v;
	return val;
}
int64_t rt_store32_idx(int64_t addr, int64_t idx, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("store32", addr, idx, 4)) return val;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return val;
	int64_t v = (val & 1) ? (val >> 1) : val;
	*(uint32_t *)p = (uint32_t)v;
	return val;
}
int64_t rt_store64_idx(int64_t addr, int64_t idx, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	if (is_int(idx)) idx >>= 1;
	if (!rt_check_oob("store64", addr, idx, 8)) return val;
	uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
	if (p < 0x1000) return val;
	*(int64_t *)p = val;
	return val;
}
int64_t rt_sys_read_off(int64_t fd, int64_t buf, int64_t len, int64_t off) {
	if (is_int(fd)) fd >>= 1;
	if (is_int(len)) len >>= 1;
	if (is_int(off)) off >>= 1;
	if (!rt_check_oob("sys_read", buf, off, (size_t)len)) return -1LL;
	ssize_t r = read((int)fd, (char *)((intptr_t)buf + (intptr_t)off), (size_t)len);
	return (int64_t)((r << 1) | 1);
}
int64_t rt_sys_write_off(int64_t fd, int64_t buf, int64_t len, int64_t off) {
	if (is_int(fd)) fd >>= 1;
	if (is_int(len)) len >>= 1;
	if (is_int(off)) off >>= 1;
	if (!rt_check_oob("sys_write", buf, off, (size_t)len)) return -1LL;
	ssize_t r = write((int)fd, (char *)((intptr_t)buf + (intptr_t)off), (size_t)len);
	return (int64_t)((r << 1) | 1);
}

int64_t rt_load8(int64_t addr) { if (!is_any_ptr(addr)) return 1; return (((int64_t)*(uint8_t *)(uintptr_t)addr) << 1) | 1; }
int64_t rt_load16(int64_t addr) { if (!is_any_ptr(addr)) return 1; return (((int64_t)*(uint16_t *)(uintptr_t)addr) << 1) | 1; }
int64_t rt_load32(int64_t addr) { if (!is_any_ptr(addr)) return 1; return (((int64_t)*(uint32_t *)(uintptr_t)addr) << 1) | 1; }
int64_t rt_load64(int64_t addr) {
	if (!is_any_ptr(addr)) return 0;
	return *(int64_t *)(uintptr_t)addr;
}

int64_t rt_store8(int64_t addr, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	int64_t v = (val & 1) ? (val >> 1) : val;
	*(uint8_t *)(uintptr_t)addr = (uint8_t)v; return val;
}
int64_t rt_store16(int64_t addr, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	int64_t v = (val & 1) ? (val >> 1) : val;
	*(uint16_t *)(uintptr_t)addr = (uint16_t)v; return val;
}
int64_t rt_store32(int64_t addr, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	int64_t v = (val & 1) ? (val >> 1) : val;
	*(uint32_t *)(uintptr_t)addr = (uint32_t)v; return val;
}
int64_t rt_store64(int64_t addr, int64_t val) {
	if (!is_any_ptr(addr)) return val;
	*(int64_t *)(uintptr_t)addr = val; return val;
}

// Optimized String Helpers
int64_t rt_to_int(int64_t v) { return v >> 1; }
int64_t rt_from_int(int64_t v) { return (v << 1) | 1; }

int64_t rt_str_concat(int64_t a, int64_t b) {
	char buf_a[512], buf_b[512];
	const char *sa = NULL, *sb = NULL;
	if (is_v_str(a)) sa = (const char *)(uintptr_t)a;
	else if (is_int(a)) { snprintf(buf_a, sizeof(buf_a), "%ld", a >> 1); sa = buf_a; }
	else if (is_v_flt(a)) { double d; memcpy(&d, (void*)(uintptr_t)a, 8); snprintf(buf_a, sizeof(buf_a), "%g", d); sa = buf_a; }
	else if (a == 2) sa = "true";
	else if (a == 4) sa = "false";
	else if (a == 0) sa = "none";
	else { snprintf(buf_a, sizeof(buf_a), "<ptr 0x%lx>", (unsigned long)a); sa = buf_a; }
	if (is_v_str(b)) sb = (const char *)(uintptr_t)b;
	else if (is_int(b)) { snprintf(buf_b, sizeof(buf_b), "%ld", b >> 1); sb = buf_b; }
	else if (is_v_flt(b)) { double d; memcpy(&d, (void*)(uintptr_t)b, 8); snprintf(buf_b, sizeof(buf_b), "%g", d); sb = buf_b; }
	else if (b == 2) sb = "true";
	else if (b == 4) sb = "false";
	else if (b == 0) sb = "none";
	else { snprintf(buf_b, sizeof(buf_b), "<ptr 0x%lx>", (unsigned long)b); sb = buf_b; }
	if (!sa || !sb) return 0;
	size_t la = strlen(sa);
	size_t lb = strlen(sb);
	int64_t res = rt_malloc((int64_t)((la + lb + 1) << 1 | 1));
	if (!res) return 0;
	*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
	*(int64_t *)(uintptr_t)((char*)res - 16) = (int64_t)(((la + lb) << 1) | 1);
	char *s = (char *)(uintptr_t)res;
	memcpy(s, sa, la);
	memcpy(s + la, sb, lb);
	s[la + lb] = '\0';
	return res;
}

int64_t rt_to_str(int64_t v) {
	if (v == 0) {
		int64_t res = rt_malloc((5 << 1) | 1);
		*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
		*(int64_t *)(uintptr_t)((char*)res - 16) = (4 << 1) | 1; // Length of "none"
		strcpy((char*)(uintptr_t)res, "none");
		return res;
	}
	if (v == 2) {
		int64_t res = rt_malloc((5 << 1) | 1);
		*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
		*(int64_t *)(uintptr_t)((char*)res - 16) = (4 << 1) | 1; // Length of "true"
		strcpy((char*)(uintptr_t)res, "true");
		return res;
	}
	if (v == 4) {
		int64_t res = rt_malloc((6 << 1) | 1);
		*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
		*(int64_t *)(uintptr_t)((char*)res - 16) = (5 << 1) | 1; // Length of "false"
		strcpy((char*)(uintptr_t)res, "false");
		return res;
	}
		if (v & 1) { // is_int (only if not 0, 2, 4 which are handled)
		int64_t val = v >> 1;
		char buf[64];
		int len = sprintf(buf, "%ld", val);
		// fprintf(stderr, "rt_to_str(int %ld) -> '%s' len %d\n", val, buf, len);
		int64_t res = rt_malloc(((int64_t)(len + 1) << 1) | 1);
		*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
		*(int64_t *)(uintptr_t)((char*)res - 16) = ((int64_t)len << 1) | 1;
		memcpy((void*)(uintptr_t)res, buf, (size_t)len + 1);
		return res;
	}
	if ((v & 3) == 2) {
		char buf[64];
		int len = sprintf(buf, "<fn 0x%lx>", (unsigned long)(v & ~3ULL));
		int64_t res = rt_malloc(((int64_t)(len + 1) << 1) | 1);
		*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
		*(int64_t *)(uintptr_t)((char*)res - 16) = ((int64_t)len << 1) | 1;
		memcpy((void*)(uintptr_t)res, buf, (size_t)len + 1);
		return res;
	}
	if (is_ptr(v)) {
		int64_t tag = *(int64_t*)((char*)(uintptr_t)v - 8);
		if (tag == TAG_STR || tag == TAG_STR_CONST) return v;
		if (tag == TAG_FLOAT) {
			double d; memcpy(&d, (void*)(uintptr_t)v, 8);
			char buf[64]; int len = sprintf(buf, "%g", d);
			int64_t res = rt_malloc(((int64_t)(len + 1) << 1) | 1);
			*(int64_t *)(uintptr_t)((char*)res - 8) = TAG_STR;
			*(int64_t *)(uintptr_t)((char*)res - 16) = ((int64_t)len << 1) | 1;
			memcpy((void*)(uintptr_t)res, buf, (size_t)len + 1);
			return res;
		}
		char buf[64];
		int len = sprintf(buf, "<ptr 0x%lx tag=%ld>", (unsigned long)v, (long)tag);
		int64_t res = rt_malloc(((int64_t)(len + 1) << 1) | 1);
		*(int64_t *)(uintptr_t)((char*)res - 8) = 120;
		*(int64_t *)(uintptr_t)((char*)res - 16) = ((int64_t)len << 1) | 1;
		memcpy((void*)(uintptr_t)res, buf, (size_t)len + 1);
		return res;
	}
	return v;
}

int64_t rt_add(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return a + b - 1;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_add(a, b);
	if (is_any_ptr(a) && is_int(b)) return a + (b >> 1);
	if (is_int(a) && is_any_ptr(b)) return b + (a >> 1);
	if (is_v_str(a) && is_v_str(b)) return rt_str_concat(a, b);
	return 1;
}

int64_t rt_sub(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return a - b + 1;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_sub(a, b);
	if (is_any_ptr(a) && is_int(b)) return a - (b >> 1);
	if (is_any_ptr(a) && is_any_ptr(b)) return ((a - b) << 1) | 1;
	return 1;
}

int64_t rt_ptr_add(int64_t a, int64_t b) {
	if (is_int(b)) b >>= 1;
	return a + b;
}
int64_t rt_ptr_sub(int64_t a, int64_t b) {
	if (is_int(b)) b >>= 1;
	return a - b;
}

int64_t rt_mul(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return (( (a >> 1) * (b >> 1) ) << 1) | 1;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_mul(a, b);
	return 0;
}

int64_t rt_div(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) {
		int64_t vb = b >> 1;
		if (vb == 0) return 0;
		return (((a >> 1) / vb) << 1) | 1;
	}
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_div(a, b);
	return 0;
}

int64_t rt_mod(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) {
		int64_t vb = b >> 1;
		if (vb == 0) return 1;
		return (((a >> 1) % vb) << 1) | 1;
	}
	return b ? a % b : 1;
}

int64_t rt_eq(int64_t a, int64_t b) {
	if (a == b) return 2; // True
	if ((a == 0 && b == 1) || (a == 1 && b == 0)) return 2; // NONE == 0
	if ((a & 1) != (b & 1)) return 4;
	if (is_ptr(a) && is_ptr(b)) {
		if (a <= 4 || b <= 4) return 4;
		if (is_v_flt(a) || is_v_flt(b)) return rt_flt_eq(a, b);
		int64_t ta = *(int64_t *)((char *)(uintptr_t)a - 8);
		int64_t tb = *(int64_t *)((char *)(uintptr_t)b - 8);
		int a_is_str = (ta == TAG_STR || ta == TAG_STR_CONST);
		int b_is_str = (tb == TAG_STR || tb == TAG_STR_CONST);
		if (a_is_str && b_is_str) {
			int res = (strcmp((const char *)(uintptr_t)a, (const char *)(uintptr_t)b) == 0);
			return res ? 2 : 4;
		}
	}
	return 4;
}

int64_t rt_lt(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return (a >> 1) < (b >> 1) ? 2 : 4;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_lt(a, b);
	if (is_ptr(a) && is_ptr(b)) return a < b ? 2 : 4;
	return 4;
}
int64_t rt_le(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return (a >> 1) <= (b >> 1) ? 2 : 4;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_le(a, b);
	if (is_ptr(a) && is_ptr(b)) return a <= b ? 2 : 4;
	return 4;
}
int64_t rt_gt(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return (a >> 1) > (b >> 1) ? 2 : 4;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_gt(a, b);
	if (is_ptr(a) && is_ptr(b)) return a > b ? 2 : 4;
	return 4;
}
int64_t rt_ge(int64_t a, int64_t b) {
	if (is_int(a) && is_int(b)) return (a >> 1) >= (b >> 1) ? 2 : 4;
	if (is_v_flt(a) || is_v_flt(b)) return rt_flt_ge(a, b);
	if (is_ptr(a) && is_ptr(b)) return a >= b ? 2 : 4;
	return 4;
}

int64_t rt_and(int64_t a, int64_t b) {
	int64_t va = (a & 1) ? (a >> 1) : a;
	int64_t vb = (b & 1) ? (b >> 1) : b;
	return ((va & vb) << 1) | 1;
}
int64_t rt_or(int64_t a, int64_t b) {
	int64_t va = (a & 1) ? (a >> 1) : a;
	int64_t vb = (b & 1) ? (b >> 1) : b;
	return ((va | vb) << 1) | 1;
}
int64_t rt_xor(int64_t a, int64_t b) {
	int64_t va = (a & 1) ? (a >> 1) : a;
	int64_t vb = (b & 1) ? (b >> 1) : b;
	return ((va ^ vb) << 1) | 1;
}
int64_t rt_shl(int64_t a, int64_t b) {
	int64_t va = (a & 1) ? (a >> 1) : a;
	int64_t vb = (b & 1) ? (b >> 1) : b;
	return ((va << vb) << 1) | 1;
}
int64_t rt_shr(int64_t a, int64_t b) {
	int64_t va = (a & 1) ? (a >> 1) : a;
	int64_t vb = (b & 1) ? (b >> 1) : b;
	return ((va >> vb) << 1) | 1;
}
int64_t rt_not(int64_t a) {
	int64_t va = (a & 1) ? (a >> 1) : a;
	return ((~va) << 1) | 1;
}

// Syscall (inline asm on x86_64 for zero overhead)

#ifdef __x86_64__
int64_t rt_syscall(int64_t n, int64_t a, int64_t b, int64_t c,
				   int64_t d, int64_t e, int64_t f) {
	long rn = (n & 1) ? (n >> 1) : n;
	long ra = a;
	long rb = b;
	long rc = c;
	long rd = (d & 1) ? (d >> 1) : d;
	long re = (e & 1) ? (e >> 1) : e;
	long rf = (f & 1) ? (f >> 1) : f;
	if (rn != 59) {
		ra = (a & 1) ? (a >> 1) : a;
		rb = (b & 1) ? (b >> 1) : b;
		rc = (c & 1) ? (c >> 1) : c;
	}
	if (rn == 59) {
		// fprintf(stderr, "SYSCALL execve: path=%lx, argv=%lx, envp=%lx\n", ra, rb, rc);
	}
	register long _num __asm__("rax") = rn;
	register long _arg1 __asm__("rdi") = ra;
	register long _arg2 __asm__("rsi") = rb;
	register long _arg3 __asm__("rdx") = rc;
	register long _arg4 __asm__("r10") = rd;
	register long _arg5 __asm__("r8") = re;
	register long _arg6 __asm__("r9") = rf;
	__asm__ __volatile__ (
		"syscall\n"
		: "+r"(_num)
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), "r"(_arg6)
		: "rcx", "r11", "memory"
	);
	return (int64_t)((_num << 1) | 1);
}
#else
int64_t rt_syscall(int64_t n, int64_t a, int64_t b, int64_t c,
				   int64_t d, int64_t e, int64_t f) {
	int64_t raw_n = (n & 1) ? (n >> 1) : n;
	int64_t raw_a = a;
	int64_t raw_b = b;
	int64_t raw_c = c;
	int64_t raw_d = (d & 1) ? (d >> 1) : d;
	int64_t raw_e = (e & 1) ? (e >> 1) : e;
	int64_t raw_f = (f & 1) ? (f >> 1) : f;
	if (raw_n != 59) {
		raw_a = (a & 1) ? (a >> 1) : a;
		raw_b = (b & 1) ? (b >> 1) : b;
		raw_c = (c & 1) ? (c >> 1) : c;
	}
	if (raw_n == 1) { // write
		// fprintf(stderr, "[SYSCALL] write(fd=%ld, buf=%p, len=%ld)\n", raw_a, (void*)b, raw_c);
	}
	int64_t res = syscall(raw_n, raw_a, raw_b, raw_c, raw_d, raw_e, raw_f);
	return (res << 1) | 1;
}
#endif

int64_t rt_execve(int64_t path, int64_t argv, int64_t envp) {
	long rpath = (path & 1) ? (path >> 1) : path;
	long rargv = (argv & 1) ? (argv >> 1) : argv;
	long renvp = (envp & 1) ? (envp >> 1) : envp;
	long res = syscall(SYS_execve, (const char *)rpath, (char *const *)rargv, (char *const *)renvp);
	return (int64_t)((res << 1) | 1);
}

// FFI (dlopen/dlsym for C library integration)

int64_t rt_dlopen(int64_t path, int64_t flags) {
	return (int64_t)dlopen((const char *)path, is_int(flags) ? (int)(flags >> 1) : (int)flags);
}

int64_t rt_dlsym(int64_t handle, int64_t name) {
	void *p = dlsym((void *)handle, (const char *)name);
	if (!p) return 0;
	return (int64_t)(uintptr_t)p | 6; // Tag 6 for Native
}

int64_t rt_dlclose(int64_t handle) {
	return dlclose((void *)handle);
}

int64_t rt_dlerror(void) {
	return (int64_t)dlerror();
}

// FFI Call Shims

int64_t rt_call0(int64_t fn) {
	if (!fn) return 1;
	if ((fn & 7) == 6) return rt_tag(((int64_t(*)(void))rt_mask_ptr(fn))());
	if ((fn & 7) == 2) return ((int64_t(*)(void))rt_mask_ptr(fn))();
	if (is_ptr(fn) && *(int64_t*)((uintptr_t)fn - 8) == 105) {
		int64_t code = *(int64_t*)((uintptr_t)fn);
		int64_t env = *(int64_t*)((uintptr_t)fn + 8);
		return ((int64_t(*)(int64_t))rt_mask_ptr(code))(env);
	}
	return ((int64_t(*)(void))fn)();
}

int64_t rt_call1(int64_t fn, int64_t a) {
	if (!fn) return 1;
	if ((fn & 7) == 6) return rt_tag(((int64_t(*)(int64_t))rt_mask_ptr(fn))(rt_untag(a)));
	if ((fn & 7) == 2) return ((int64_t(*)(int64_t))rt_mask_ptr(fn))(a);
	if (is_ptr(fn) && *(int64_t*)((uintptr_t)fn - 8) == 105) {
		int64_t code = *(int64_t*)((uintptr_t)fn);
		int64_t env = *(int64_t*)((uintptr_t)fn + 8);
		return ((int64_t(*)(int64_t, int64_t))rt_mask_ptr(code))(env, a);
	}
	return ((int64_t(*)(int64_t))fn)(a);
}

int64_t rt_call2(int64_t fn, int64_t a, int64_t b) {
	if (!fn) return 1;
	if ((fn & 7) == 6) return rt_tag(((int64_t(*)(int64_t, int64_t))rt_mask_ptr(fn))(rt_untag(a), rt_untag(b)));
	if ((fn & 7) == 2) return ((int64_t(*)(int64_t, int64_t))rt_mask_ptr(fn))(a, b);
	if (is_ptr(fn) && *(int64_t*)((uintptr_t)fn - 8) == 105) {
		int64_t code = *(int64_t*)((uintptr_t)fn);
		int64_t env = *(int64_t*)((uintptr_t)fn + 8);
		return ((int64_t(*)(int64_t, int64_t, int64_t))rt_mask_ptr(code))(env, a, b);
	}
	return ((int64_t(*)(int64_t, int64_t))fn)(a, b);
}

int64_t rt_call3(int64_t fn, int64_t a, int64_t b, int64_t c) {
	if (!fn) return 1;
	if ((fn & 7) == 6) return rt_tag(((int64_t(*)(int64_t, int64_t, int64_t))rt_mask_ptr(fn))(rt_untag(a), rt_untag(b), rt_untag(c)));
	if ((fn & 7) == 2) {
		typedef int64_t (*f3)(int64_t, int64_t, int64_t);
		f3 target = (f3)rt_mask_ptr(fn);
		return target(a, b, c);
	}
	if (is_ptr(fn) && *(int64_t*)((uintptr_t)fn - 8) == 105) {
		int64_t code = *(int64_t*)((uintptr_t)fn);
		int64_t env = *(int64_t*)((uintptr_t)fn + 8);
		return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(code))(env, a, b, c);
	}
	return ((int64_t(*)(int64_t, int64_t, int64_t))fn)(a, b, c);
}

int64_t rt_call4(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d) {
	if (!fn) return 1;
	if ((fn & 7) == 6) return rt_tag(((int64_t(*)(int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(fn))(rt_untag(a), rt_untag(b), rt_untag(c), rt_untag(d)));
	if ((fn & 7) == 2) return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(fn))(a, b, c, d);
	if (is_ptr(fn) && *(int64_t*)((uintptr_t)fn - 8) == 105) {
		int64_t code = *(int64_t*)((uintptr_t)fn);
		int64_t env = *(int64_t*)((uintptr_t)fn + 8);
		return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(code))(env, a, b, c, d);
	}
	return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t))fn)(a, b, c, d);
}

int64_t rt_call5(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
	if (!fn) return 1;
	if ((fn & 7) == 6) return rt_tag(((int64_t(*)(int64_t, int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(fn))(rt_untag(a), rt_untag(b), rt_untag(c), rt_untag(d), rt_untag(e)));
	if ((fn & 7) == 2) return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(fn))(a, b, c, d, e);
	if (is_ptr(fn) && *(int64_t*)((uintptr_t)fn - 8) == 105) {
		int64_t code = *(int64_t*)((uintptr_t)fn);
		int64_t env = *(int64_t*)((uintptr_t)fn + 8);
		return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(code))(env, a, b, c, d, e);
	}
	return ((int64_t(*)(int64_t, int64_t, int64_t, int64_t, int64_t))fn)(a, b, c, d, e);
}

int64_t rt_call6(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g) {
	if (!fn) return 1;
	if ((fn & 7) == 2 || (fn & 7) == 6) {
		 int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
		 return f(a, b, c, d, e, g);
	}
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)fn;
	return f(a, b, c, d, e, g);
}

int64_t rt_call7(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h);
}

int64_t rt_call8(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h, int64_t i) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h, i);
}

int64_t rt_call9(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h, int64_t i, int64_t j) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h, i, j);
}

int64_t rt_call10(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h, int64_t i, int64_t j, int64_t k) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h, i, j, k);
}

int64_t rt_call11(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h, int64_t i, int64_t j, int64_t k, int64_t l) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h, i, j, k, l);
}

int64_t rt_call12(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h, int64_t i, int64_t j, int64_t k, int64_t l, int64_t m) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h, i, j, k, l, m);
}

int64_t rt_call13(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t g, int64_t h, int64_t i, int64_t j, int64_t k, int64_t l, int64_t m, int64_t n) {
	int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
	return f(a, b, c, d, e, g, h, i, j, k, l, m, n);
}

// Exit

__attribute__((noreturn))
int64_t rt_exit(int64_t code) {
	if (is_int(code)) code >>= 1;
	exit((int)code);
}

// Threads (pthreads-backed; works on glibc/musl/mingw-w64 pthreads)

typedef struct rt_thread_arg {
	int64_t fn;
	int64_t arg;
} rt_thread_arg;

static void *rt_thread_trampoline(void *p) {
	rt_thread_arg *ta = (rt_thread_arg *)p;
	int64_t fn = ta->fn;
	int64_t arg = ta->arg;
	free(ta);
	int64_t (*f)(int64_t) = (void *)(uintptr_t)rt_mask_ptr(fn);
	int64_t res = f(arg);
	return (void *)(uintptr_t)res;
}

int64_t rt_thread_spawn(int64_t fn, int64_t arg) {
	pthread_t tid;
	rt_thread_arg *ta = malloc(sizeof(rt_thread_arg));
	if (!ta) return -1;
	ta->fn = fn;
	ta->arg = arg;
	int r = pthread_create(&tid, NULL, rt_thread_trampoline, ta);
	if (r != 0) { free(ta); return -r; }
	return (int64_t)tid;
}

int64_t rt_thread_join(int64_t tid) {
	void *ret = NULL;
	int r = pthread_join((pthread_t)tid, &ret);
	if (r != 0) return -r;
	return (int64_t)(uintptr_t)ret;
}

int64_t rt_mutex_new(void) {
	pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
	if (!m) return 0;
	if (pthread_mutex_init(m, NULL) != 0) { free(m); return 0; }
	return (int64_t)(uintptr_t)m;
}

int64_t rt_mutex_lock64(int64_t m) {
	if (!m) return -1;
	return pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)m);
}

int64_t rt_mutex_unlock64(int64_t m) {
	if (!m) return -1;
	return pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)m);
}

int64_t rt_mutex_free(int64_t m) {
	if (!m) return 0;
	pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)m);
	free((void *)(uintptr_t)m);
	return 0;
}

// Process args (host -> JIT bridge)

int64_t rt_set_args(int64_t argc, int64_t argv_ptr, int64_t envp_ptr) {
	g_argc = (int)argc;
	g_argv = calloc(g_argc + 1, sizeof(char *));
	char **old_argv = (char **)argv_ptr;
	for (int i = 0; i < g_argc; i++) {
		if (old_argv[i]) {
			size_t len = strlen(old_argv[i]);
			// Use rt_malloc to ensure alignment and tracking
			int64_t p = rt_malloc((int64_t)((len + 1) << 1 | 1));
			*(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
			*(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
			memcpy((void *)(uintptr_t)p, old_argv[i], len + 1);
			g_argv[i] = (char *)(uintptr_t)p;
		} else {
			g_argv[i] = NULL;
		}
	}
	g_argv[g_argc] = NULL;
	char **old_envp = (char **)envp_ptr;
	int env_count = 0;
	if (old_envp) {
		while (old_envp[env_count]) env_count++;
	}
	g_envc = env_count;
	g_envp = calloc(env_count + 1, sizeof(char *));
					for (int i = 0; i < env_count; i++) {
						size_t len = strlen(old_envp[i]);
						int64_t p = rt_malloc((int64_t)((len + 1) << 1 | 1));
						*(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
						*(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
						memcpy((void *)(uintptr_t)p, old_envp[i], len + 1);
						g_envp[i] = (char *)(uintptr_t)p;
					}
					g_envp[env_count] = NULL;	return 0;
}

void rt_cleanup_args(void) {
	if (g_argv) {
		for (int i = 0; i < g_argc; i++) {
			if (g_argv[i]) rt_free((int64_t)(uintptr_t)g_argv[i]);
		}
		free(g_argv);
		g_argv = NULL;
		g_argc = 0;
	}
	if (g_envp) {
		for (int i = 0; i < g_envc; i++) {
			if (g_envp[i]) rt_free((int64_t)(uintptr_t)g_envp[i]);
		}
		free(g_envp);
		g_envp = NULL;
		g_envc = 0;
	}
}

int64_t rt_argc(void) { return (int64_t)((g_argc << 1) | 1); }
int64_t rt_envc(void) { return (int64_t)((g_envc << 1) | 1); }

int64_t rt_argv(int64_t i) {
	if (!is_int(i)) return 0;
	int idx = (int)(i >> 1);
	if (idx < 0 || idx >= g_argc) return 0;
	const char *s = g_argv[idx];
	size_t len = strlen(s);
	int64_t res = rt_malloc(((int64_t)(len + 1) << 1) | 1);
	*(int64_t *)((char *)(uintptr_t)res - 8) = TAG_STR;
	*(int64_t *)((char *)(uintptr_t)res - 16) = ((int64_t)len << 1) | 1;
	strcpy((char *)(uintptr_t)res, s);
	return res;
}

int64_t rt_init_str(int64_t obj, int64_t len) {
	if (!is_heap_ptr(obj)) return 0;
	*(int64_t *)((char *)(uintptr_t)obj - 8) = TAG_STR;
	*(int64_t *)((char *)(uintptr_t)obj - 16) = len | 1;
	return obj;
}

int64_t rt_envp(void) {
	return (int64_t)g_envp;
}

// Debug Stats (only in debug builds with NYTRIX_MEM_STATS=1)

#ifndef NDEBUG
#include <stdio.h>
__attribute__((destructor))
static void rt_stats(void) {
	if (getenv("NYTRIX_MEM_STATS")) {
		fprintf(stderr, "\n━━━ Nytrix Runtime Stats ━━━\n");
		fprintf(stderr, "Allocated: %lu bytes\n", g_alloc);
		fprintf(stderr, "Freed:     %lu bytes\n", g_free);
		fprintf(stderr, "Leaked:    %ld bytes\n", (long)(g_alloc - g_free));
		fprintf(stderr, "Pool hits: %lu (%.1f%%)\n",
				g_pool_hits, g_alloc ? 100.0 * g_pool_hits / g_alloc : 0);
	}
}
#endif

#include <sys/socket.h>
#include <arpa/inet.h>

int64_t rt_recv(int64_t sockfd, int64_t buf, int64_t len, int64_t flags) {
	if (is_int(sockfd)) sockfd >>= 1;
	if (is_int(len)) len >>= 1;
	if (is_int(flags)) flags >>= 1;
	if (!buf) return -1;
	ssize_t res = recv((int)sockfd, (void *)(uintptr_t)buf, (size_t)len, (int)flags);
	return (int64_t)((res << 1) | 1);
}

int64_t rt_kwarg(int64_t k, int64_t v) {
	int64_t res = rt_malloc(16);
	if (!res) return 0;
	*(int64_t *)(uintptr_t)((char*)res - 8) = 209; // Tag 104 (209 raw)
	((int64_t *)(uintptr_t)res)[0] = k;
	((int64_t *)(uintptr_t)res)[1] = v;
	return res;
}

// Floating Point Primitives (Soft-Float via Double Bits)

int64_t rt_flt_from_int(int64_t v) {
	if (is_int(v)) {
		double d = (double)(v >> 1);
		int64_t res;
		memcpy(&res, &d, 8);
		return res;
	}
	return 0; // Should not happen if correctly used
}

int64_t rt_flt_to_int(int64_t v) {
	int64_t b = rt_flt_unbox_val(v);
	double d;
	memcpy(&d, &b, 8);
	return ((int64_t)d << 1) | 1;
}

int64_t rt_flt_trunc(int64_t v) {
	return rt_flt_to_int(v);
}

int64_t rt_flt_add(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	double r = da + db;
	int64_t rr;
	memcpy(&rr, &r, 8);
	return rt_flt_box_val(rr);
}

int64_t rt_flt_sub(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	double r = da - db;
	int64_t rr;
	memcpy(&rr, &r, 8);
	return rt_flt_box_val(rr);
}

int64_t rt_flt_mul(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	double r = da * db;
	int64_t rr;
	memcpy(&rr, &r, 8);
	return rt_flt_box_val(rr);
}

int64_t rt_flt_div(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	double r = da / db;
	int64_t rr;
	memcpy(&rr, &r, 8);
	return rt_flt_box_val(rr);
}

int64_t rt_flt_lt(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	return (da < db) ? 2 : 4;
}

int64_t rt_flt_gt(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	return (da > db) ? 2 : 4;
}

int64_t rt_flt_eq(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	return (da == db) ? 2 : 4;
}

int64_t rt_flt_le(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	return (da <= db) ? 2 : 4;
}

int64_t rt_flt_ge(int64_t a, int64_t b) {
	double da, db;
	int64_t ba = rt_flt_unbox_val(a);
	int64_t bb = rt_flt_unbox_val(b);
	memcpy(&da, &ba, 8);
	memcpy(&db, &bb, 8);
	return (da >= db) ? 2 : 4;
}
