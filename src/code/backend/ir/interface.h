/*
 * Explicit backend emitter interface dispatch for Nytrix native target
 * backends.
 */
#ifndef NYTRIX_NATIVE_BACKEND_INTERFACE_H
#define NYTRIX_NATIVE_BACKEND_INTERFACE_H

#include "code/backend/internal.h"
typedef struct ny_backend_emitter {
  const char *name;
  nyr_backend_target_t target;
  bool (*emit_nir)(nyr_writer_t *w, const ny_native_target_info_t *target,
                   const nyr_func_t *nyir, const char *label, bool tag_return,
                   char *err, size_t err_len);
  bool (*emit_mach_scalar)(nyr_writer_t *w,
                           const ny_native_target_info_t *target,
                           const nyr_mach_func_t *mach, const char *label,
                           bool tag_return, char *err, size_t err_len);
} nyr_backend_emitter_t;

const nyr_backend_emitter_t *
nyr_backend_find_emitter(nyr_backend_target_t target);

#endif /* NYTRIX_NATIVE_BACKEND_INTERFACE_H */
