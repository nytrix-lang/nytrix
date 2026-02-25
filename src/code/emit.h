#ifndef NY_CODEGEN_EMIT_H
#define NY_CODEGEN_EMIT_H

#include "code/code.h"

void ny_codegen_emit_defers(codegen_t *cg, scope *scopes, size_t depth,
                            size_t func_root);
void ny_codegen_emit_top_functions(codegen_t *cg, stmt_t *s, scope *gsc,
                                   size_t gd, const char *cur_mod);

#endif
