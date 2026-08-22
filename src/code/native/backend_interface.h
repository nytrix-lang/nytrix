/*
 * Explicit backend emitter interface dispatch for Nytrix native target backends.
 */
#ifndef NYTRIX_NATIVE_BACKEND_INTERFACE_H
#define NYTRIX_NATIVE_BACKEND_INTERFACE_H

#include "code/native/internal.h"

typedef struct ny_backend_emitter {
  const char *name;
  ny_native_target_t target;
  bool (*emit_nir)(ny_native_writer_t *w,
                   const ny_native_target_info_t *target,
                   const nyir_func_t *nyir,
                   const char *label,
                   bool tag_return,
                   char *err,
                   size_t err_len);
  bool (*emit_mach_scalar)(ny_native_writer_t *w,
                           const ny_native_target_info_t *target,
                           const ny_mach_func_t *mach,
                           const char *label,
                           bool tag_return,
                           char *err,
                           size_t err_len);
} ny_backend_emitter_t;

const ny_backend_emitter_t *ny_backend_find_emitter(ny_native_target_t target);

#endif /* NYTRIX_NATIVE_BACKEND_INTERFACE_H */
