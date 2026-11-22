/*
 * ny-perf bench-pack: benchmark evidence collection with assembly generation.
 * Replaces etc/tools/bench_pack.py in C for performance and distribution.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "perf.h"
#include "base/args.h"
#include "base/json.h"
#include "base/util.h"
#include "base/sha256.h"
#include "base/ui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char route[16];
    char fixture[256];
    char argv[1024];
    int exit_code;
    double seconds;
    char log[PATH_MAX];
    int cached;
    char error[256];
} BenchPackRecord;

typedef struct {
    BenchPackRecord *items;
    size_t len;
    size_t cap;
} BenchPackRecords;

static void records_push(BenchPackRecords *r, const BenchPackRecord *rec) {
    if (r->len == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 64;
        BenchPackRecord *p = realloc(r->items, nc * sizeof(BenchPackRecord));
        if (!p) return;
        r->items = p;
        r->cap = nc;
    }
    r->items[r->len++] = *rec;
}

static void records_free(BenchPackRecords *r) {
    free(r->items);
    r->items = NULL;
    r->len = r->cap = 0;
}

static int assemble_fixture(const char *fixtures_dir,
                            const char *name, const char *out_dir,
                            const char *ny_bin, const char *nyc_bin,
                            const char *objdump, int timeout_sec,
                            char *const envp[], const char *cache_root,
                            int asm_refresh, BenchPackRecords *records);

/*
 * Parallel assembly worker
 */
typedef struct {
    const char *fixtures_dir;
    const char *out_dir;
    const char *ny_bin;
    const char *nyc_bin;
    const char *objdump;
    int timeout_sec;
    const char *cache_root;
    int asm_refresh;
    BenchPackRecords *records;
    pthread_mutex_t *records_mutex;
    char **selection;
    size_t selection_count;
    size_t *next_index;
    int asm_jobs;
    int *completed_count;
    pthread_mutex_t *completed_mutex;
} AsmWorkerArgs;

static void *asm_worker(void *arg) {
    AsmWorkerArgs *args = (AsmWorkerArgs *)arg;
    while (1) {
        size_t idx;
        pthread_mutex_t *idx_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(idx_mutex, NULL);
        pthread_mutex_lock(idx_mutex);
        if (*args->next_index >= args->selection_count) {
            pthread_mutex_unlock(idx_mutex);
            pthread_mutex_destroy(idx_mutex);
            free(idx_mutex);
            break;
        }
        idx = (*args->next_index)++;
        pthread_mutex_unlock(idx_mutex);
        pthread_mutex_destroy(idx_mutex);
        free(idx_mutex);

        assemble_fixture(args->fixtures_dir, args->selection[idx], args->out_dir,
                         args->ny_bin, args->nyc_bin, args->objdump, args->timeout_sec,
                         NULL, args->cache_root, args->asm_refresh, args->records);

        pthread_mutex_lock(args->completed_mutex);
        (*args->completed_count)++;
        int completed = *args->completed_count;
        pthread_mutex_unlock(args->completed_mutex);

        nyt_msg("ASM", NYT_CYAN, "%d/%zu %s", completed, args->selection_count, args->selection[idx]);
    }
    return NULL;
}

static void run_assembly_parallel(const char *fixtures_dir, char **selection, size_t selection_count,
                                   const char *out_dir, const char *ny_bin, const char *nyc_bin,
                                   const char *objdump, int timeout_sec, const char *cache_root,
                                   int asm_refresh, int asm_jobs, BenchPackRecords *records) {
    if (selection_count == 0) return;

    int actual_jobs = asm_jobs < (int)selection_count ? asm_jobs : (int)selection_count;
    if (actual_jobs <= 1) {
        /*
         * Sequential fallback
         */
        for (size_t i = 0; i < selection_count; i++) {
            assemble_fixture(fixtures_dir, selection[i], out_dir,
                             ny_bin, nyc_bin, objdump, timeout_sec,
                             NULL, cache_root, asm_refresh, records);
            nyt_msg("ASM", NYT_CYAN, "%zu/%zu %s", i + 1, selection_count, selection[i]);
        }
        return;
    }

    pthread_t *threads = malloc(actual_jobs * sizeof(pthread_t));
    AsmWorkerArgs args = {
        .fixtures_dir = fixtures_dir,
        .out_dir = out_dir,
        .ny_bin = ny_bin,
        .nyc_bin = nyc_bin,
        .objdump = objdump,
        .timeout_sec = timeout_sec,
        .cache_root = cache_root,
        .asm_refresh = asm_refresh,
        .records = records,
        .records_mutex = malloc(sizeof(pthread_mutex_t)),
        .selection = selection,
        .selection_count = selection_count,
        .next_index = malloc(sizeof(size_t)),
        .asm_jobs = actual_jobs,
        .completed_count = malloc(sizeof(int)),
        .completed_mutex = malloc(sizeof(pthread_mutex_t)),
    };
    *args.next_index = 0;
    *args.completed_count = 0;
    pthread_mutex_init(args.records_mutex, NULL);
    pthread_mutex_init(args.completed_mutex, NULL);

    for (int i = 0; i < actual_jobs; i++) {
        pthread_create(&threads[i], NULL, asm_worker, &args);
    }

    for (int i = 0; i < actual_jobs; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    pthread_mutex_destroy(args.records_mutex);
    free(args.records_mutex);
    pthread_mutex_destroy(args.completed_mutex);
    free(args.completed_mutex);
    free(args.next_index);
    free(args.completed_count);
}

static char *read_small_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return 0;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return 0;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                ok = 0;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }
    if (n < 0) ok = 0;
    if (close(out) != 0) ok = 0;
    close(in);
    return ok;
}

static int extract_shape_source(const char *shape_path, const char *lang,
                                const char *tag, const char *out_path) {
    char *src = read_small_file(shape_path);
    if (!src) return 0;
    char marker[64];
    snprintf(marker, sizeof(marker), "source %s <<'%s'", lang, tag);
    char *start = strstr(src, marker);
    if (!start) { free(src); return 0; }
    start += strlen(marker);
    if (*start == '\r') start++;
    if (*start == '\n') start++;
    char end_marker[16];
    snprintf(end_marker, sizeof(end_marker), "\n%s\n", tag);
    char *end = strstr(start, end_marker);
    if (!end) { free(src); return 0; }
    FILE *f = fopen(out_path, "w");
    if (!f) { free(src); return 0; }
    fwrite(start, 1, (size_t)(end - start), f);
    fputc('\n', f);
    fclose(f);
    free(src);
    return 1;
}

/*
 * removed: unused run_cmd_live
 */

static int run_cmd_capture(char *const argv[], const char *log_path,
                           int timeout_sec, char *const envp[], double *elapsed_ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        int fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (envp) {
            for (int i = 0; envp[i]; i++) {
                char *eq = strchr(envp[i], '=');
                if (eq) {
                    *eq = '\0';
                    setenv(envp[i], eq + 1, 1);
                    *eq = '=';
                }
            }
        }
        for (int i = 0; argv[i]; i++) {
            if (strpbrk(argv[i], ";|&$`\n")) _exit(127);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return 1;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_s = (double)(now.tv_sec - t0.tv_sec) +
                           (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed_s >= (double)timeout_sec) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            if (elapsed_ms) *elapsed_ms = elapsed_s * 1000.0;
            return 2;
        }
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (elapsed_ms) {
        *elapsed_ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
                      ((double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static char *sha256_hex(const void *data, size_t len) {
    /*
     * Use FNV-1a for cache keys (fast, non-crypto)
     */
    uint64_t hash = 14695981039346656037ULL;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    char *hex = malloc(17);
    if (!hex) return NULL;
    snprintf(hex, 17, "%016llx", (unsigned long long)hash);
    return hex;
}

static char *sha256_file(const char *path) {
    char out[72];
    if (!ny_sha256_file(path, out, sizeof(out))) return NULL;
    char *hex = malloc(72);
    if (!hex) return NULL;
    snprintf(hex, 72, "%s", out);
    return hex;
}

static char *shutil_which(const char *cmd);

static void discover_fixtures_recursive(const char *dir, const char *base, char ***selection,
                                        size_t *selection_count, size_t *selection_cap,
                                        const char *pattern);

static int assemble_fixture(const char *fixtures_dir,
                            const char *name, const char *out_dir,
                            const char *ny_bin, const char *nyc_bin,
                            const char *objdump, int timeout_sec,
                            char *const envp[], const char *cache_root,
                            int asm_refresh, BenchPackRecords *records) {
    char shape_path[PATH_MAX];
    snprintf(shape_path, sizeof(shape_path), "%s/%s.nshape", fixtures_dir, name);
    struct stat st;
    if (stat(shape_path, &st) != 0) return 0;

    char dest_dir[PATH_MAX];
    snprintf(dest_dir, sizeof(dest_dir), "%s/disassembly", out_dir);
    if (!ny_mkdir_p(dest_dir)) {
        return 0;
    }
    char flat_name[PATH_MAX];
    snprintf(flat_name, sizeof(flat_name), "%s", name);
    for (char *p = flat_name; *p; p++) if (*p == '/') *p = '_';
    char full_dest[PATH_MAX];
    snprintf(full_dest, sizeof(full_dest), "%s/%s", dest_dir, flat_name);
    if (!ny_mkdir_p(full_dest)) {
        return 0;
    }
    char *dest_dir_ptr = full_dest;

    char ny_src[PATH_MAX], c_src[PATH_MAX];
    snprintf(ny_src, sizeof(ny_src), "%s/input.ny", dest_dir_ptr);
    snprintf(c_src, sizeof(c_src), "%s/input.c", dest_dir_ptr);

    char *shape_data = read_small_file(shape_path);
    if (shape_data) {
        char marker[64];
        snprintf(marker, sizeof(marker), "source ny <<'");
        char *start = strstr(shape_data, marker);
        if (start) {
            start += strlen(marker);
            char *tag_end = strchr(start, '\'');
            if (tag_end) {
                *tag_end = '\0';
                char tag[64];
                snprintf(tag, sizeof(tag), "%s", start);
                *tag_end = '\'';
                extract_shape_source(shape_path, "ny", tag, ny_src);
            }
        }
        snprintf(marker, sizeof(marker), "source c <<'");
        start = strstr(shape_data, marker);
        if (start) {
            start += strlen(marker);
            char *tag_end = strchr(start, '\'');
            if (tag_end) {
                *tag_end = '\0';
                char tag[64];
                snprintf(tag, sizeof(tag), "%s", start);
                *tag_end = '\'';
                extract_shape_source(shape_path, "c", tag, c_src);
            }
        }
        free(shape_data);
    }

    int has_ny = access(ny_src, F_OK) == 0;
    int has_c = access(c_src, F_OK) == 0;

    struct {
        const char *route;
        const char *obj;
        const char *flags;
        const char *compiler;
    } specs[3];
    int spec_count = 0;

    if (has_ny) {
        specs[spec_count].route = "nyr";
        specs[spec_count].obj = "nyr.o";
        specs[spec_count].flags = "--native-only";
        specs[spec_count].compiler = ny_bin;
        spec_count++;
        specs[spec_count].route = "llvm";
        specs[spec_count].obj = "llvm.o";
        specs[spec_count].flags = "";
        specs[spec_count].compiler = ny_bin;
        spec_count++;
    }
    if (has_c) {
        specs[spec_count].route = "nyc";
        specs[spec_count].obj = "nyc.o";
        specs[spec_count].flags = "-O3 --all";
        specs[spec_count].compiler = nyc_bin;
        spec_count++;
    }

    for (int i = 0; i < spec_count; i++) {
        char obj_path[PATH_MAX], asm_path[PATH_MAX], log_path[PATH_MAX];
        snprintf(obj_path, sizeof(obj_path), "%s/%s", dest_dir_ptr, specs[i].obj);
        snprintf(asm_path, sizeof(asm_path), "%s/%s.asm", dest_dir_ptr, specs[i].route);
        snprintf(log_path, sizeof(log_path), "%s/%s.compile.log", dest_dir_ptr, specs[i].route);

        struct stat compiler_st;
        stat(specs[i].compiler, &compiler_st);

        const char *source = strcmp(specs[i].route, "nyc") == 0
                        ? c_src : ny_src;
        char *source_hash = sha256_file(source);
        char key_input[2048];
        long compiler_mtime = (long)compiler_st.st_mtime;
        int key_len = snprintf(key_input, sizeof(key_input),
                               "%s %s %ld %ld %s %s",
                               specs[i].route, specs[i].flags,
                               compiler_st.st_size, compiler_mtime,
                               source_hash ? source_hash : "", objdump ? objdump : "");
        free(source_hash);

        char *key_hash = sha256_hex(key_input, key_len);
        char cache_dir[PATH_MAX];
        snprintf(cache_dir, sizeof(cache_dir), "%s/%s", cache_root, key_hash);
        free(key_hash);

        int cached = 0;
        if (!asm_refresh) {
            char cached_asm[PATH_MAX], cached_log[PATH_MAX];
            snprintf(cached_asm, sizeof(cached_asm), "%s/output.asm", cache_dir);
            snprintf(cached_log, sizeof(cached_log), "%s/compile.log", cache_dir);
            if (access(cached_asm, F_OK) == 0 && access(cached_log, F_OK) == 0) {
                ny_mkdir_p(dest_dir_ptr);
                if (!copy_file(cached_asm, asm_path) || !copy_file(cached_log, log_path)) {
                    /*
                     * copy failed, fall through to recompile
                     */
                } else {
                BenchPackRecord rec = {0};
                snprintf(rec.route, sizeof(rec.route), "%s", specs[i].route);
                snprintf(rec.fixture, sizeof(rec.fixture), "%s", name);
                if (strcmp(specs[i].route, "nyc") == 0) {
                    snprintf(rec.argv, sizeof(rec.argv), "%s --all -O3 %s -o %s",
                             specs[i].compiler, c_src, obj_path);
                } else {
                    snprintf(rec.argv, sizeof(rec.argv), "%s %s -O3 %s --output=%s %s",
                             specs[i].compiler, specs[i].flags,
                             strcmp(specs[i].route, "nyr") == 0 ? "--native-only" : "",
                             obj_path, has_ny ? ny_src : c_src);
                }
                rec.exit_code = 0;
                rec.seconds = 0;
                snprintf(rec.log, sizeof(rec.log), "%s", log_path);
                rec.cached = 1;
                records_push(records, &rec);
                cached = 1;
            }
            }
        }

        if (!cached) {
            char *compile_argv[16];
            int argc = 0;
            bool is_nyc = strcmp(specs[i].route, "nyc") == 0;
            bool is_nyr = strcmp(specs[i].route, "nyr") == 0;

            compile_argv[argc++] = (char *)specs[i].compiler;
            compile_argv[argc++] = "-O3";

            if (is_nyc) {
                compile_argv[argc++] = "--all";
                compile_argv[argc++] = c_src;
                compile_argv[argc++] = obj_path;
            } else {
                compile_argv[argc++] = "--no-progress";
                compile_argv[argc++] = is_nyr
                    ? "--native-backend=x86_64"
                    : "--native-backend=llvm";
                if (is_nyr)
                    compile_argv[argc++] = "--native-only";
                compile_argv[argc++] = "--run=none";
                compile_argv[argc++] = "--output";
                compile_argv[argc++] = obj_path;
                compile_argv[argc++] = ny_src;
            }
            compile_argv[argc] = NULL;

            double elapsed = 0;
            int rc = run_cmd_capture(compile_argv, log_path, timeout_sec, envp, &elapsed);

            BenchPackRecord rec = {0};
            snprintf(rec.route, sizeof(rec.route), "%s", specs[i].route);
            snprintf(rec.fixture, sizeof(rec.fixture), "%s", name);
            char *argv_str = rec.argv;
            for (int j = 0; compile_argv[j]; j++) {
                argv_str += snprintf(argv_str, sizeof(rec.argv) - (argv_str - rec.argv),
                                     "%s ", compile_argv[j]);
            }
            rec.exit_code = rc;
            rec.seconds = round(elapsed / 1000.0 * 1000) / 1000.0;
            snprintf(rec.log, sizeof(rec.log), "%s", log_path);
            rec.cached = 0;
            records_push(records, &rec);

            if (rc == 0 && objdump && access(obj_path, F_OK) == 0) {
                char disasm_log[PATH_MAX];
                snprintf(disasm_log, sizeof(disasm_log), "%s/%s.disasm.log", dest_dir_ptr, specs[i].route);
                char *disasm_argv[] = {(char *)objdump, "-dr", "-Mintel", obj_path, NULL};
                int drc = run_cmd_capture(disasm_argv, asm_path, timeout_sec, NULL, NULL);
                if (drc != 0) {
                    /*
                     * keep asm path empty on failure; log retained
                     */
                    unlink(asm_path);
                    (void)disasm_log;
                } else {
                    ny_mkdir_p(cache_dir);
                    char cached_asm[PATH_MAX], cached_log[PATH_MAX];
                    snprintf(cached_asm, sizeof(cached_asm), "%s/output.asm", cache_dir);
                    snprintf(cached_log, sizeof(cached_log), "%s/compile.log", cache_dir);
                    copy_file(asm_path, cached_asm);
                    copy_file(log_path, cached_log);
                }
            }
            unlink(obj_path);
        }
    }
    return 1;
}

static void write_manifest(const char *out_dir, BenchPackRecords *records,
                           const char *pattern,
                           int runs, int timeout, int jobs, int asm_jobs,
                           int asm_all, const char *asm_cache) {
    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", out_dir);
    FILE *f = fopen(manifest_path, "w");
    if (!f) return;

    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 1,\n");
    fprintf(f, "  \"generated_utc\": \"%s\",\n", timestamp);
    struct utsname uts;
    const char *platform = "unknown";
    const char *machine = "unknown";
    if (uname(&uts) == 0) {
        platform = uts.sysname;
        machine = uts.machine;
    }
    fprintf(f, "  \"host\": {\n");
    fprintf(f, "    \"platform\": \"%s\",\n", platform);
    fprintf(f, "    \"machine\": \"%s\",\n", machine);
    fprintf(f, "    \"python\": \"N/A\"\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"settings\": {\n");
    fprintf(f, "    \"runs\": %d,\n", runs);
    fprintf(f, "    \"timeout\": %d,\n", timeout);
    fprintf(f, "    \"pattern\": \"%s\",\n", pattern ? pattern : "");
    fprintf(f, "    \"opt\": 3,\n");
    fprintf(f, "    \"jobs\": %d,\n", jobs);
    fprintf(f, "    \"asm_jobs\": %d,\n", asm_jobs);
    fprintf(f, "    \"asm_all\": %s,\n", asm_all ? "true" : "false");
    fprintf(f, "    \"asm_cache\": \"%s\",\n", asm_cache);
    fprintf(f, "    \"concurrent_asm\": true\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"routes\": {\n");
    fprintf(f, "    \"native\": \"NYR benchmark runner with LLVM/C/NYC comparisons\",\n");
    fprintf(f, "    \"nyr\": \"NYR relocatable object disassembly\",\n");
    fprintf(f, "    \"llvm\": \"LLVM relocatable object disassembly\",\n");
    fprintf(f, "    \"nyc\": \"NYC C frontend relocatable object disassembly\"\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"commands\": [\n");

    for (size_t i = 0; i < records->len; i++) {
        BenchPackRecord *r = &records->items[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"route\": \"%s\",\n", r->route);
        if (r->fixture[0]) fprintf(f, "      \"fixture\": \"%s\",\n", r->fixture);
        fprintf(f, "      \"argv\": [");
        char *argv_copy = strdup(r->argv);
        char *token = strtok(argv_copy, " ");
        int first = 1;
        while (token) {
            if (!first) fprintf(f, ", ");
            fprintf(f, "\"%s\"", token);
            first = 0;
            token = strtok(NULL, " ");
        }
        free(argv_copy);
        fprintf(f, "],\n");
        fprintf(f, "      \"exit_code\": %d,\n", r->exit_code);
        fprintf(f, "      \"seconds\": %.3f,\n", r->seconds);
        fprintf(f, "      \"log\": \"%s\"", r->log);
        if (r->cached) fprintf(f, ",\n      \"cached\": true");
        if (r->error[0]) fprintf(f, ",\n      \"error\": \"%s\"", r->error);
        fprintf(f, "\n    }%s\n", (i + 1 < records->len) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

static void write_readme(const char *out_dir, BenchPackRecords *records) {
    char readme_path[PATH_MAX];
    snprintf(readme_path, sizeof(readme_path), "%s/README.md", out_dir);
    FILE *f = fopen(readme_path, "w");
    if (!f) return;

    int failures = 0;
    for (size_t i = 0; i < records->len; i++) {
        if (records->items[i].exit_code) failures++;
    }

    fprintf(f, "# Nytrix benchmark evidence\n\n");
    fprintf(f, "These results describe the build host, not the recipient host.\n");
    fprintf(f, "Compare checksums and failures before performance. Do not compare unvalidated timings.\n");
    fprintf(f, "Assembly generation may run concurrently with benchmarks and perturb timings.\n");
    fprintf(f, "The native benchmark report already compares NYR, LLVM, C, and NYC.\n");
    fprintf(f, "The NYC artifacts are compile/disassembly evidence, not NYC runtime speed measurements.\n\n");
    fprintf(f, "## Summary\n\n");
    fprintf(f, "- Commands: %zu\n", records->len);
    fprintf(f, "- Failures: %d\n", failures);
    fprintf(f, "- Benchmark report: `native/`\n");
    fprintf(f, "- Native/LLVM/NYC assembly: `disassembly/`\n");
    fprintf(f, "- Complete command lines, exit codes, durations: `manifest.json`\n\n");
    fprintf(f, "## Failures\n\n");

    int has_failures = 0;
    for (size_t i = 0; i < records->len; i++) {
        BenchPackRecord *r = &records->items[i];
        if (r->exit_code) {
            has_failures = 1;
            fprintf(f, "- `%s` %s: exit %d, `%s`\n",
                    r->route, r->fixture[0] ? r->fixture : "(suite)",
                    r->exit_code, r->log);
        }
    }
    if (!has_failures) fprintf(f, "None\n");
    fclose(f);
}

int ny_perf_bench_main(int argc, char **argv) {
    /*
     * Skip mode name if present (called from ny_perf_main with mode as argv[1])
     */
    int arg_offset = 0;
    if (argc > 1 && (strcmp(argv[1], "bench") == 0 || strcmp(argv[1], "bench-pack") == 0)) {
        arg_offset = 1;
    }
    int new_argc = argc - arg_offset;
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    if (!new_argv) return 2;
    new_argv[0] = argv[0];
    for (int i = 0; i < new_argc - 1; i++) {
        new_argv[i + 1] = argv[arg_offset + 1 + i];
    }
    new_argv[new_argc] = NULL;

    char root[PATH_MAX] = {0};
    char out_dir[PATH_MAX] = {0};
    char ny_bin[PATH_MAX] = {0};
    char ny_test_bin[PATH_MAX] = {0};
    char nyc_bin[PATH_MAX] = {0};
    const char *root_ptr = NULL;
    const char *out_dir_ptr = NULL;
    const char *ny_bin_ptr = NULL;
    const char *ny_test_bin_ptr = NULL;
    const char *nyc_bin_ptr = NULL;
    int runs = 3;
    int jobs = 1;
    int timeout_sec = 120;
    char pattern[256] = {0};
    int asm_all = 0;
    int asm_jobs = 6;
    char asm_cache[PATH_MAX] = {0};
    int asm_refresh = 0;
    char err[256];

    for (int i = 1; i < new_argc; i++) {
        const char *a = new_argv[i];
        int color_mode = -2;
        int color_idx = i;
        int color_rc = ny_arg_consume_color(&color_idx, new_argc, new_argv, &color_mode, err, sizeof(err));
        if (color_rc < 0) { free(new_argv); nyt_err("ny-perf", "%s", err); return 2; }
        if (color_rc > 0) { ny_arg_apply_color_mode(color_mode); i = color_idx; continue; }

        if (ny_arg_match_with_value(a, "--root")) {
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &root_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (root_ptr) snprintf(root, sizeof(root), "%s", root_ptr);
        } else if (ny_arg_match_with_value(a, "--out")) {
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &out_dir_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (out_dir_ptr) snprintf(out_dir, sizeof(out_dir), "%s", out_dir_ptr);
        } else if (ny_arg_match_with_value(a, "--ny")) {
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &ny_bin_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (ny_bin_ptr) snprintf(ny_bin, sizeof(ny_bin), "%s", ny_bin_ptr);
        } else if (ny_arg_match_with_value(a, "--ny-test")) {
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &ny_test_bin_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (ny_test_bin_ptr) snprintf(ny_test_bin, sizeof(ny_test_bin), "%s", ny_test_bin_ptr);
        } else if (ny_arg_match_with_value(a, "--nyc")) {
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &nyc_bin_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (nyc_bin_ptr) snprintf(nyc_bin, sizeof(nyc_bin), "%s", nyc_bin_ptr);
        } else if (ny_arg_match_with_value(a, "--runs")) {
            if (!ny_arg_take_int(a, &i, new_argc, new_argv, 1, 10, &runs, "runs", err, sizeof(err))) { free(new_argv); return 2; }
        } else if (ny_arg_match_with_value(a, "--jobs")) {
            if (!ny_arg_take_int(a, &i, new_argc, new_argv, 1, 256, &jobs, "jobs", err, sizeof(err))) { free(new_argv); return 2; }
        } else if (ny_arg_match_with_value(a, "--timeout")) {
            if (!ny_arg_take_int(a, &i, new_argc, new_argv, 1, 3600, &timeout_sec, "timeout", err, sizeof(err))) { free(new_argv); return 2; }
        } else if (ny_arg_match_with_value(a, "--pattern")) {
            const char *pattern_ptr = NULL;
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &pattern_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (pattern_ptr) snprintf(pattern, sizeof(pattern), "%s", pattern_ptr);
        } else if (strcmp(a, "--asm-all") == 0) {
            asm_all = 1;
        } else if (ny_arg_match_with_value(a, "--asm-jobs")) {
            if (!ny_arg_take_int(a, &i, new_argc, new_argv, 1, 256, &asm_jobs, "asm-jobs", err, sizeof(err))) { free(new_argv); return 2; }
        } else if (ny_arg_match_with_value(a, "--asm-cache")) {
            const char *asm_cache_ptr = NULL;
            if (!ny_arg_take_value(a, &i, new_argc, new_argv, &asm_cache_ptr, err, sizeof(err))) { free(new_argv); return 2; }
            if (asm_cache_ptr) snprintf(asm_cache, sizeof(asm_cache), "%s", asm_cache_ptr);
        } else if (strcmp(a, "--asm-refresh") == 0) {
            asm_refresh = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("Usage: ny perf bench-pack --root ROOT --out OUT --ny NY --ny-test NY_TEST --nyc NYC [options]\n");
            printf("Options:\n");
            printf("  --runs N           Number of benchmark runs (default: 3)\n");
            printf("  --jobs N           Benchmark worker jobs (default: 1)\n");
            printf("  --timeout SEC      Timeout per benchmark (default: 120)\n");
            printf("  --pattern PAT      Filter fixtures by pattern\n");
            printf("  --asm-all          Generate assembly for all discovered fixtures\n");
            printf("  --asm-jobs N       Assembly worker jobs (default: 6)\n");
            printf("  --asm-cache DIR    Assembly cache directory\n");
            printf("  --asm-refresh      Ignore cache and regenerate\n");
            printf("Default ASM selection: 4 representative fixtures, or --pattern matches, or --asm-all.\n");
            free(new_argv);
            return 0;
        } else {
            free(new_argv);
            nyt_err("ny-perf", "unknown option: %s", a);
            return 2;
        }
    }

    free(new_argv);

    if (!root[0] || !out_dir[0] || !ny_bin[0] || !ny_test_bin[0] || !nyc_bin[0]) {
        nyt_err("ny-perf", "missing required arguments (--root, --out, --ny, --ny-test, --nyc)");
        return 2;
    }

    if (chdir(root) != 0) {
        nyt_err("ny-perf", "could not chdir to %s", root);
        return 1;
    }

    if (!asm_cache[0]) {
        snprintf(asm_cache, sizeof(asm_cache), "%s/build/bench-asm-cache", root);
    }
    ny_mkdir_p(asm_cache);

    char fixtures_dir[PATH_MAX];
    snprintf(fixtures_dir, sizeof(fixtures_dir), "%s/etc/labs/tests/bench", root);

    /*
     * Discover fixtures for assembly evidence.
     * - --asm-all: assemble every discovered fixture (filtered by --pattern).
     * - --pattern without --asm-all: assemble only matching fixtures.
     * - default: assemble a small representative set to keep tar evidence fast.
     */
    char **asm_selection = NULL;
    size_t asm_selection_count = 0, asm_selection_cap = 0;
    if (asm_all || pattern[0]) {
        discover_fixtures_recursive(fixtures_dir, "", &asm_selection,
                                    &asm_selection_count, &asm_selection_cap, pattern);
    } else {
        const char *defaults[] = {"startup/cmov-sort", "startup/loop",
                                  "numeric/intops", "algorithms/prime"};
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            if (asm_selection_count == asm_selection_cap) {
                asm_selection_cap = asm_selection_cap ? asm_selection_cap * 2 : 8;
                char **grown = realloc(asm_selection, asm_selection_cap * sizeof(char *));
                if (!grown) {
                    fprintf(stderr, "bench: out of memory selecting asm fixtures\n");
                    exit(1);
                }
                asm_selection = grown;
            }
            asm_selection[asm_selection_count++] = strdup(defaults[i]);
        }
    }

    qsort(asm_selection, asm_selection_count, sizeof(char *),
          (int (*)(const void *, const void *))strcmp);

    char *objdump = getenv("OBJDUMP");
    if (!objdump) objdump = "objdump";
    if (!shutil_which(objdump)) objdump = "llvm-objdump";
    if (!shutil_which(objdump)) objdump = NULL;

    nyt_msg("BENCH", NYT_CYAN, "%d workers · %d runs · assembly saved to archive", jobs, runs);
    nyt_msg("ASM", NYT_CYAN, "%zu fixtures · %d workers · cached; starting alongside benchmarks",
            asm_selection_count, asm_jobs < (int)asm_selection_count ? asm_jobs : (int)asm_selection_count);

    BenchPackRecords records = {0};

    /*
     * Run assembly generation in parallel
     */
    run_assembly_parallel(fixtures_dir, asm_selection, asm_selection_count,
                          out_dir, ny_bin, nyc_bin, objdump, timeout_sec,
                          asm_cache, asm_refresh, asm_jobs, &records);

    /*
     * Run benchmarks
     */
    char native_dir[PATH_MAX];
    snprintf(native_dir, sizeof(native_dir), "%s/native", out_dir);
    ny_mkdir_p(native_dir);

    char runs_s[16], jobs_s[16], timeout_s[16];
    snprintf(runs_s, sizeof(runs_s), "%d", runs);
    snprintf(jobs_s, sizeof(jobs_s), "%d", jobs);
    snprintf(timeout_s, sizeof(timeout_s), "%d", timeout_sec);
    char json_out[PATH_MAX], csv_out[PATH_MAX], md_out[PATH_MAX];
    snprintf(json_out, sizeof(json_out), "%s/results.json", native_dir);
    snprintf(csv_out, sizeof(csv_out), "%s/results.csv", native_dir);
    snprintf(md_out, sizeof(md_out), "%s/report.md", native_dir);
    char *bench_argv[64];
    int bargc = 0;
    bench_argv[bargc++] = ny_test_bin;
    bench_argv[bargc++] = "--bin";
    bench_argv[bargc++] = ny_bin;
    bench_argv[bargc++] = "--bench";
    bench_argv[bargc++] = "--jobs";
    bench_argv[bargc++] = jobs_s;
    bench_argv[bargc++] = "--bench-engine";
    bench_argv[bargc++] = "native";
    bench_argv[bargc++] = "--bench-opt";
    bench_argv[bargc++] = "O3";
    bench_argv[bargc++] = "--bench-run";
    bench_argv[bargc++] = runs_s;
    bench_argv[bargc++] = "--bench-warmup";
    bench_argv[bargc++] = "1";
    bench_argv[bargc++] = "--bench-timeout";
    bench_argv[bargc++] = timeout_s;
    bench_argv[bargc++] = "--bench-correctness";
    bench_argv[bargc++] = "--bench-code-metrics";
    bench_argv[bargc++] = "--bench-compare-llvm";
    bench_argv[bargc++] = "--bench-out-json";
    bench_argv[bargc++] = json_out;
    bench_argv[bargc++] = "--bench-out-csv";
    bench_argv[bargc++] = csv_out;
    bench_argv[bargc++] = "--bench-report-md";
    bench_argv[bargc++] = md_out;
    bench_argv[bargc++] = "--bench-evidence";
    if (pattern[0]) {
        bench_argv[bargc++] = "--pattern";
        bench_argv[bargc++] = pattern;
    }
    bench_argv[bargc] = NULL;

    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/run.log", native_dir);
    double bench_elapsed = 0;
    int rc = run_cmd_capture(bench_argv, log_path, timeout_sec, NULL, &bench_elapsed);

    BenchPackRecord bench_rec = {0};
    snprintf(bench_rec.route, sizeof(bench_rec.route), "native");
    char *argv_str = bench_rec.argv;
    size_t argv_used = 0;
    for (int j = 0; bench_argv[j] && argv_used + 1 < sizeof(bench_rec.argv); j++) {
        int n = snprintf(argv_str + argv_used, sizeof(bench_rec.argv) - argv_used,
                         "%s%s", j ? " " : "", bench_argv[j]);
        if (n < 0) break;
        if ((size_t)n >= sizeof(bench_rec.argv) - argv_used) {
            argv_used = sizeof(bench_rec.argv) - 1;
            break;
        }
        argv_used += (size_t)n;
    }
    bench_rec.exit_code = rc;
    bench_rec.seconds = round(bench_elapsed / 1000.0 * 1000.0) / 1000.0;
    snprintf(bench_rec.log, sizeof(bench_rec.log), "%s", log_path);
    records_push(&records, &bench_rec);

    write_manifest(out_dir, &records, pattern, runs, timeout_sec, jobs, asm_jobs,
                   asm_all, asm_cache);
    for (size_t i = 0; i < asm_selection_count; i++) free(asm_selection[i]);
    free(asm_selection);
    write_readme(out_dir, &records);

    int failures = 0;
    for (size_t i = 0; i < records.len; i++) if (records.items[i].exit_code) failures++;

    nyt_msg("BENCH", NYT_GREEN, "bench evidence: %s (%zu commands, %d failures)", out_dir, records.len, failures);

    records_free(&records);

    return failures ? 1 : 0;
}

static char *shutil_which(const char *cmd) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = strdup(path);
    char *save = NULL;
    for (char *p = strtok_r(copy, ":", &save); p; p = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", p, cmd);
        if (access(full, X_OK) == 0) {
            free(copy);
            return strdup(full);
        }
    }
    free(copy);
    return NULL;
}

static void discover_fixtures_recursive(const char *dir, const char *base, char ***selection,
                                        size_t *selection_count, size_t *selection_cap,
                                        const char *pattern) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char new_base[PATH_MAX];
            if (base[0]) {
                snprintf(new_base, sizeof(new_base), "%s/%s", base, ent->d_name);
            } else {
                snprintf(new_base, sizeof(new_base), "%s", ent->d_name);
            }
            discover_fixtures_recursive(path, new_base, selection, selection_count, selection_cap, pattern);
        } else if (S_ISREG(st.st_mode)) {
            if (strstr(ent->d_name, ".nshape")) {
                size_t len = strlen(ent->d_name);
                if (len > 7 && strcmp(ent->d_name + len - 7, ".nshape") == 0) {
                    char *name;
                    if (base[0]) {
                        size_t base_len = strlen(base);
                        name = malloc(base_len + 1 + len - 6);
                        if (!name) continue;
                        snprintf(name, base_len + 1 + len - 6, "%s/%.*s", base, (int)(len - 7), ent->d_name);
                    } else {
                        name = malloc(len - 6);
                        if (!name) continue;
                        memcpy(name, ent->d_name, len - 7);
                        name[len - 7] = '\0';
                    }
                    if (pattern[0] && !strcasestr(name, pattern)) {
                        free(name);
                        continue;
                    }
                    if (*selection_count == *selection_cap) {
                        size_t grown_cap = *selection_cap ? *selection_cap * 2 : 64;
                        char **grown = realloc(*selection, grown_cap * sizeof(char *));
                        if (!grown) {
                            free(name);
                            continue;
                        }
                        *selection = grown;
                        *selection_cap = grown_cap;
                    }
                    (*selection)[(*selection_count)++] = name;
                }
            }
        }
    }
    closedir(d);
}
