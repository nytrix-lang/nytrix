#ifndef NY_DBG_H
#define NY_DBG_H

// Forward declarations
typedef struct codegen_t codegen_t;
typedef struct token_t token_t;

void ny_dbg_loc(codegen_t *cg, token_t tok);

#endif // NY_DBG_H
