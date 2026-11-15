/* x86-64 AST fallback emitter interfaces for generated runtime entry points. */
#ifndef NY_CODE_BACKEND_ARCH_X86_64_AST_H
#define NY_CODE_BACKEND_ARCH_X86_64_AST_H

#include "code/backend/core/internal.h"
#include "code/lang/ny/parser/ast.h"

bool nyr_x86_64_emit_rt_main(nyr_writer_t *w,
                             const ny_native_target_info_t *target,
                             const program_t *prog, char *err, size_t err_len);

#endif
