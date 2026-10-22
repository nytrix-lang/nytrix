#include <nytrix/nytrix.h>

#include "base/options.h"
#include "base/util.h"
#include "code/backend/ir/internal.h"
#include "code/lang/pipeline.h"
#include "code/lang/runtime/gc.h"
#include "code/lang/runtime/shared.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

struct nytrix_value_handle {
  int64_t value;
};

static atomic_flag nytrix_embed_state_lock = ATOMIC_FLAG_INIT;
static bool nytrix_embed_runtime_started;
static size_t nytrix_embed_live_handles;
static size_t nytrix_embed_active_runs;

static void nytrix_embed_lock(void) {
  while (atomic_flag_test_and_set_explicit(&nytrix_embed_state_lock,
                                           memory_order_acquire)) {
  }
}

static void nytrix_embed_unlock(void) {
  atomic_flag_clear_explicit(&nytrix_embed_state_lock, memory_order_release);
}

static void nytrix_embed_error(char *err, size_t err_len, const char *message) {
  if (err && err_len)
    snprintf(err, err_len, "%s", message ? message : "embedding error");
}

bool nytrix_embed_startup(char *err, size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
  nytrix_embed_lock();
  if (!nytrix_embed_runtime_started) {
    nyGcInit();
    nytrix_embed_runtime_started = true;
  }
  nytrix_embed_unlock();
  return true;
}

bool nytrix_embed_shutdown(char *err, size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
  nytrix_embed_lock();
  if (!nytrix_embed_runtime_started) {
    nytrix_embed_unlock();
    return true;
  }
  if (nytrix_embed_live_handles || nytrix_embed_active_runs) {
    nytrix_embed_unlock();
    nytrix_embed_error(err, err_len,
                       "cannot shut down while runs or value handles are live");
    return false;
  }
  nyr_eval_globals_clear();
  (void)rt_runtime_cleanup();
  nyGcDispose();
  nytrix_embed_runtime_started = false;
  nytrix_embed_unlock();
  return true;
}

bool nytrix_value_handle_create(nytrix_value_t value,
                                nytrix_value_handle_t **out, char *err,
                                size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
  if (!out) {
    nytrix_embed_error(err, err_len, "value handle output is null");
    return false;
  }
  *out = NULL;
  nytrix_value_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) {
    nytrix_embed_error(err, err_len, "cannot allocate value handle");
    return false;
  }
  handle->value = value;
  nytrix_embed_lock();
  if (!nytrix_embed_runtime_started) {
    nyGcInit();
    nytrix_embed_runtime_started = true;
  }
  nyGcAddRoot(&handle->value);
  nytrix_embed_live_handles++;
  nytrix_embed_unlock();
  *out = handle;
  return true;
}

bool nytrix_value_handle_get(const nytrix_value_handle_t *handle,
                             nytrix_value_t *out, char *err, size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
  if (!handle || !out) {
    nytrix_embed_error(err, err_len, "value handle or output is null");
    return false;
  }
  nytrix_embed_lock();
  if (!nytrix_embed_runtime_started) {
    nytrix_embed_unlock();
    nytrix_embed_error(err, err_len, "embedding runtime is shut down");
    return false;
  }
  *out = nyGcReadRoot(&handle->value);
  nytrix_embed_unlock();
  return true;
}

bool nytrix_value_handle_set(nytrix_value_handle_t *handle,
                             nytrix_value_t value, char *err, size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
  if (!handle) {
    nytrix_embed_error(err, err_len, "value handle is null");
    return false;
  }
  nytrix_embed_lock();
  if (!nytrix_embed_runtime_started) {
    nytrix_embed_unlock();
    nytrix_embed_error(err, err_len, "embedding runtime is shut down");
    return false;
  }
  nyGcWriteBarrier(&handle->value, value);
  nytrix_embed_unlock();
  return true;
}

void nytrix_value_handle_release(nytrix_value_handle_t *handle) {
  if (!handle)
    return;
  nytrix_embed_lock();
  nyGcRemoveRoot(&handle->value);
  if (nytrix_embed_live_handles)
    nytrix_embed_live_handles--;
  nytrix_embed_unlock();
  free(handle);
}

bool nytrix_value_handle_set_nil(nytrix_value_handle_t *handle, char *err,
                                 size_t err_len) {
  return nytrix_value_handle_set(handle, NY_IMM_NIL, err, err_len);
}

bool nytrix_value_handle_set_bool(nytrix_value_handle_t *handle, bool value,
                                  char *err, size_t err_len) {
  return nytrix_value_handle_set(handle, value ? NY_IMM_TRUE : NY_IMM_FALSE,
                                 err, err_len);
}

bool nytrix_value_handle_get_bool(const nytrix_value_handle_t *handle,
                                  bool *out, char *err, size_t err_len) {
  nytrix_value_t value = 0;
  if (!out || !nytrix_value_handle_get(handle, &value, err, err_len)) {
    if (!out)
      nytrix_embed_error(err, err_len, "boolean output is null");
    return false;
  }
  if (!rt_is_bool_imm(value)) {
    nytrix_embed_error(err, err_len, "Ny value is not a boolean");
    return false;
  }
  *out = rt_is_true_imm(value);
  return true;
}

bool nytrix_value_handle_set_i64(nytrix_value_handle_t *handle, int64_t value,
                                 char *err, size_t err_len) {
  int64_t boxed = ny_value_from_i64(value);
  if (!boxed) {
    nytrix_embed_error(err, err_len, "cannot allocate Ny integer value");
    return false;
  }
  return nytrix_value_handle_set(handle, boxed, err, err_len);
}

bool nytrix_value_handle_get_i64(const nytrix_value_handle_t *handle,
                                 int64_t *out, char *err, size_t err_len) {
  nytrix_value_t value = 0;
  if (!out || !nytrix_value_handle_get(handle, &value, err, err_len)) {
    if (!out)
      nytrix_embed_error(err, err_len, "integer output is null");
    return false;
  }
  if (!rt_bigint_to_i64_checked(value, out)) {
    nytrix_embed_error(err, err_len, "Ny value is not a signed 64-bit integer");
    return false;
  }
  return true;
}

bool nytrix_value_handle_set_f64(nytrix_value_handle_t *handle, double value,
                                 char *err, size_t err_len) {
  int64_t boxed = rt_flt_box_double(value);
  if (!boxed) {
    nytrix_embed_error(err, err_len, "cannot allocate Ny floating-point value");
    return false;
  }
  return nytrix_value_handle_set(handle, boxed, err, err_len);
}

bool nytrix_value_handle_get_f64(const nytrix_value_handle_t *handle,
                                 double *out, char *err, size_t err_len) {
  nytrix_value_t value = 0;
  if (!out || !nytrix_value_handle_get(handle, &value, err, err_len)) {
    if (!out)
      nytrix_embed_error(err, err_len, "floating-point output is null");
    return false;
  }
  if (rt_value_tag(value) != TAG_FLOAT) {
    nytrix_embed_error(err, err_len, "Ny value is not a floating-point value");
    return false;
  }
  *out = rt_flt_unbox_double(value);
  return true;
}

bool nytrix_value_handle_set_string(nytrix_value_handle_t *handle,
                                    const char *data, size_t length, char *err,
                                    size_t err_len) {
  if (!data && length) {
    nytrix_embed_error(err, err_len, "string data is null");
    return false;
  }
  if (length > (size_t)(INT64_MAX / 2 - 1)) {
    nytrix_embed_error(err, err_len, "string is too large to marshal");
    return false;
  }
  int64_t boxed = rt_alloc_string_len(data ? data : "", length);
  if (!boxed) {
    nytrix_embed_error(err, err_len, "cannot allocate Ny string value");
    return false;
  }
  return nytrix_value_handle_set(handle, boxed, err, err_len);
}

bool nytrix_value_handle_get_string(const nytrix_value_handle_t *handle,
                                    char *buffer, size_t capacity,
                                    size_t *length, char *err, size_t err_len) {
  nytrix_value_t value = 0;
  if (!length || !nytrix_value_handle_get(handle, &value, err, err_len)) {
    if (!length)
      nytrix_embed_error(err, err_len, "string length output is null");
    return false;
  }
  if (!is_v_str(value)) {
    nytrix_embed_error(err, err_len, "Ny value is not a string");
    return false;
  }
  size_t required = rt_tagged_str_len(value);
  *length = required;
  if (!buffer && capacity == 0)
    return true;
  if (!buffer || capacity < required) {
    nytrix_embed_error(err, err_len, "string output buffer is too small");
    return false;
  }
  if (required)
    memcpy(buffer, (const void *)(uintptr_t)value, required);
  return true;
}

/*
 * Compiler and runtime state are process-global. A recursive source run from
 * a same-thread host call can reuse that state because each pipeline's
 * compiler objects are stack-local; other threads must not overlap a run.
 */
#if defined(_MSC_VER)
#define NYTRIX_EMBED_TLS __declspec(thread)
#else
#define NYTRIX_EMBED_TLS _Thread_local
#endif
static atomic_flag nytrix_embed_run_active = ATOMIC_FLAG_INIT;
static NYTRIX_EMBED_TLS size_t nytrix_embed_run_depth;

static bool nytrix_embed_run_begin(void) {
  nytrix_embed_lock();
  if (!nytrix_embed_runtime_started) {
    nyGcInit();
    nytrix_embed_runtime_started = true;
  }
  if (nytrix_embed_run_depth == 0 &&
      atomic_flag_test_and_set(&nytrix_embed_run_active)) {
    nytrix_embed_unlock();
    return false;
  }
  nytrix_embed_run_depth++;
  nytrix_embed_active_runs++;
  nytrix_embed_unlock();
  return true;
}
static void nytrix_embed_run_end(void) {
  nytrix_embed_lock();
  if (nytrix_embed_run_depth && --nytrix_embed_run_depth == 0)
    atomic_flag_clear(&nytrix_embed_run_active);
  if (nytrix_embed_active_runs)
    nytrix_embed_active_runs--;
  nytrix_embed_unlock();
}

static int nytrix_embed_run_options(const char *source, const char *path,
                                    const nytrix_embed_run_config_t *config) {
  if (source ? (path || !*source) : (!path || !*path))
    return -1;
  if (config &&
      (config->opt_level < 0 || config->opt_level > 3 || config->reserved))
    return -1;
  if (!nytrix_embed_run_begin())
    return -1;
  ny_options opt;
  ny_options_init(&opt);
  opt.mode = NY_MODE_RUN;
  opt.run_jit = true;
  opt.command_string = source;
  opt.input_file = path;
  opt.opt_level = config ? config->opt_level : 0;
  opt.no_progress = true;
  /*
   * The pipeline returns the program status directly. The run-depth guard
   * keeps the active flag set until the outermost recursive run ends.
   */
  int exit_code = ny_pipeline_run(&opt);
  nytrix_embed_run_end();
  return exit_code;
}

int nytrix_embed_run_source(const char *source,
                            const nytrix_embed_run_config_t *config) {
  return nytrix_embed_run_options(source, NULL, config);
}

int nytrix_embed_run_file(const char *path,
                          const nytrix_embed_run_config_t *config) {
  return nytrix_embed_run_options(NULL, path, config);
}

/*
 * Reserve a unique artifact path under the system temp directory. The file is
 * created (and immediately unlinked) so the pipeline's artifact writer owns a
 * name nothing else can claim between reservation and write.
 */
static bool nytrix_embed_artifact_path(char *path, size_t cap) {
  const char *dir = ny_get_temp_dir();
  if (!dir || !*dir)
    return false;
#ifdef _WIN32
  for (int attempt = 0; attempt < 100; ++attempt) {
    int n = snprintf(path, cap, "%snytrix-embed-compile-%llu-%d.json", dir,
                     (unsigned long long)ny_ticks_now(), attempt);
    if (n <= 0 || (size_t)n >= cap)
      return false;
    FILE *probe = fopen(path, "wx");
    if (probe) {
      fclose(probe);
      remove(path);
      return true;
    }
  }
  return false;
#else
  int n = snprintf(path, cap, "%s/nytrix-embed-compile-XXXXXX", dir);
  if (n <= 0 || (size_t)n >= cap)
    return false;
  int fd = mkstemp(path);
  if (fd < 0)
    return false;
  close(fd);
  unlink(path);
  return true;
#endif
}

void nytrix_embed_compile_artifact_free(char *artifact_json) {
  free(artifact_json);
}

int nytrix_embed_compile_source(const char *source,
                                const char *options_json_or_null,
                                char **artifact_json_out) {
  if (artifact_json_out)
    *artifact_json_out = NULL;
  if (!source || !*source || !artifact_json_out)
    return -1;
  /*
   * The options document is reserved; fail closed on anything but an empty
   * request so future options cannot be silently ignored.
   */
  if (options_json_or_null && *options_json_or_null)
    return -1;
  if (!nytrix_embed_run_begin())
    return -1;
  char artifact_path[4096] = {0};
  int exit_code =
      nytrix_embed_artifact_path(artifact_path, sizeof(artifact_path)) ? 0 : -1;
  if (exit_code == 0) {
    ny_options opt;
    ny_options_init(&opt);
    opt.mode = NY_MODE_RUN;
    opt.command_string = source;
    opt.no_progress = true;
    /*
     * Stop after the type pipeline and write the semantic artifact: the same
     * path as `ny --stop-after=hm --emit-artifact`, without execution.
     */
    opt.stop_after = NY_STOP_AFTER_HM;
    opt.emit_artifact_path = artifact_path;
    exit_code = ny_pipeline_run(&opt);
    if (exit_code == 0) {
      char *json = ny_read_file(artifact_path);
      if (json)
        *artifact_json_out = json;
      else
        exit_code = -1;
    }
    remove(artifact_path);
  }
  nytrix_embed_run_end();
  return exit_code;
}
