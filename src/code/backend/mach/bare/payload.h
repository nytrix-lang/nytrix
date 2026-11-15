/* Private opcode mapping for the embedded freestanding runtime payload. */
#ifndef NY_BARE_PAYLOAD_H
#define NY_BARE_PAYLOAD_H
#include "code/backend/core/abi.h"

/* Private payload ABI. Public runtime-op enum ordinals may change freely. */
static inline unsigned ny_bare_payload_op(ny_native_runtime_op_t op,
                                          unsigned *argc) {
  unsigned id = 0, n = 1;
  switch (op) {
  case NY_NATIVE_RUNTIME_OP_I64_TO_CSTR:
    id = 1;
    break;
  case NY_NATIVE_RUNTIME_OP_F64_TO_CSTR:
    id = 2;
    break;
  case NY_NATIVE_RUNTIME_OP_PRINT_F64:
    id = 3;
    break;
  case NY_NATIVE_RUNTIME_OP_ANY_TO_CSTR:
    id = 4;
    break;
  case NY_NATIVE_RUNTIME_OP_F64_BITS:
    id = 5;
    break;
  case NY_NATIVE_RUNTIME_OP_FLT_BOX_VAL:
    id = 6;
    break;
  case NY_NATIVE_RUNTIME_OP_TAG:
    id = 7;
    break;
  case NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_CLONE:
    id = 15;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_REPLACE:
    id = 8;
    n = 3;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_SLICE:
    id = 9;
    n = 3;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_NEW:
    id = 10;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_APPEND:
    id = 11;
    n = 2;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_APPEND_KNOWN:
    id = 12;
    n = 3;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_REPEAT_KNOWN:
    id = 13;
    n = 4;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_FINALIZE:
    id = 14;
    break;
  case NY_NATIVE_RUNTIME_OP_VALUE_TAG:
    id = 16;
    break;
  case NY_NATIVE_RUNTIME_OP_CSTR_REVERSE:
    id = 17;
    break;
  default:
    break;
  }
  if (argc)
    *argc = n;
  return id;
}
#endif
