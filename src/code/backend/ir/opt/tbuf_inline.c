/*
 * Tbuf inline pass: replaces core.tbuf typed-buffer calls with direct memory
 * access.  Each rewrite mirrors the source helper in lib/tbuf.ny: the index is
 * scaled by the element width, the buffer base is added, and the access keeps
 * the element width in its instruction flags/scalar class.  Store helpers
 * return the buffer, so a store rewrite publishes the base through a COPY when
 * the call result is used.
 */
#include "tbuf_inline.h"
#include "code/backend/ir/opt/util.h"
#include "code/backend/ir/internal.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
  TBUF_FN_NONE = 0,
  TBUF_FN_U8,
  TBUF_FN_I32,
  TBUF_FN_I64,
  TBUF_FN_F32,
  TBUF_FN_F64,
} tbuf_fn_t;

typedef struct {
  const char *name;
  tbuf_fn_t kind;
  bool store;
} tbuf_entry_t;

static const tbuf_entry_t TBUF_ENTRIES[] = {
    {"core.tbuf.u8buf_load", TBUF_FN_U8, false},
    {"core.tbuf.i32buf_load", TBUF_FN_I32, false},
    {"core.tbuf.i64buf_load", TBUF_FN_I64, false},
    {"core.tbuf.f32buf_load", TBUF_FN_F32, false},
    {"core.tbuf.f64buf_load", TBUF_FN_F64, false},
    {"core.tbuf.u8buf_store", TBUF_FN_U8, true},
    {"core.tbuf.i32buf_store", TBUF_FN_I32, true},
    {"core.tbuf.i64buf_store", TBUF_FN_I64, true},
    {"core.tbuf.f32buf_store", TBUF_FN_F32, true},
    {"core.tbuf.f64buf_store", TBUF_FN_F64, true},
};

static tbuf_fn_t tbuf_classify(const char *name, bool *store) {
  if (!name)
    return TBUF_FN_NONE;
  for (size_t i = 0; i < sizeof(TBUF_ENTRIES) / sizeof(TBUF_ENTRIES[0]); ++i) {
    if (strcmp(name, TBUF_ENTRIES[i].name) == 0) {
      if (store)
        *store = TBUF_ENTRIES[i].store;
      return TBUF_ENTRIES[i].kind;
    }
  }
  return TBUF_FN_NONE;
}

static unsigned tbuf_elem_size(tbuf_fn_t kind) {
  switch (kind) {
  case TBUF_FN_U8:
    return 1;
  case TBUF_FN_I32:
  case TBUF_FN_F32:
    return 4;
  case TBUF_FN_I64:
  case TBUF_FN_F64:
    return 8;
  default:
    return 0;
  }
}

static nyr_inst_t tbuf_make(nyr_op_t op, int dst, int a, int b, int c) {
  nyr_inst_t in = {0};
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = b;
  in.c = c;
  in.d = -1;
  in.e = -1;
  in.f = -1;
  in.cmp = NYIR_CMP_EQ;
  in.effects = nyr_inst_effects(&in);
  return in;
}

static nyr_inst_t tbuf_make_const(int dst, int64_t value) {
  nyr_inst_t in = tbuf_make(NYIR_CONST_I64, dst, -1, -1, -1);
  in.imm = value;
  return in;
}

/*
 * Splice `inst` into the stream at `pos`, shifting everything else up.
 */
static bool tbuf_insert(nyr_func_t *f, size_t pos, const nyr_inst_t *inst) {
  if (!nir_ensure_inst_space(f, 1))
    return false;
  memmove(&f->data[pos + 1], &f->data[pos],
          (f->len - pos) * sizeof(*f->data));
  f->data[pos] = *inst;
  f->len++;
  return true;
}

bool nyr_tbuf_inline(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;

  /*
   * Rewrite from the end so earlier call indices stay valid while later
   * instructions are spliced in.  The pass reports success even when nothing
   * matched; only an allocation failure aborts the pipeline.
   */
  for (size_t i = f->len; i-- > 0;) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    bool store = false;
    tbuf_fn_t kind = tbuf_classify(in->symbol, &store);
    if (kind == TBUF_FN_NONE)
      continue;

    int buffer = in->a;
    int index = in->b;
    int value = in->c;
    int dst = in->dst;
    int64_t arity = in->imm;
    if (buffer < 0 || index < 0 || (store && value < 0))
      continue;
    if (arity != (store ? 3 : 2))
      continue;
    if (!store && dst < 0)
      continue;

    unsigned elem = tbuf_elem_size(kind);
    if (elem == 0)
      continue;
    unsigned mem_flags = 0;
    nyr_scalar_class_t scalar = NYIR_SCALAR_I64_RAW;
    switch (kind) {
    case TBUF_FN_U8:
      mem_flags = NYIR_INST_F_MEM_BYTE;
      break;
    case TBUF_FN_I32:
      mem_flags = NYIR_INST_F_MEM_I32;
      break;
    case TBUF_FN_F32:
      scalar = NYIR_SCALAR_F32;
      break;
    case TBUF_FN_F64:
      scalar = NYIR_SCALAR_F64;
      break;
    default:
      break;
    }

    nyr_debug_loc_t debug = in->debug;
    size_t extra = elem == 1 ? 1 : 3;
    if (store && dst >= 0)
      extra++;
    if (!nir_ensure_inst_space(f, extra))
      return false;

    int address = f->next_value++;
    size_t call_at = i;
    if (elem == 1) {
      /*
       * u8 buffers index in bytes: address = buffer + index.
       */
      nyr_inst_t add = tbuf_make(NYIR_ADD_I64, address, buffer, index, -1);
      add.debug = debug;
      if (!tbuf_insert(f, call_at, &add))
        return false;
      call_at++;
    } else {
      /*
       * address = buffer + (index << log2(elem)).
       */
      unsigned shift = elem == 4 ? 2 : 3;
      int amount = f->next_value++;
      int offset = f->next_value++;
      nyr_inst_t amount_inst = tbuf_make_const(amount, (int64_t)shift);
      nyr_inst_t shift_inst =
          tbuf_make(NYIR_SHL_I64, offset, index, amount, -1);
      nyr_inst_t add = tbuf_make(NYIR_ADD_I64, address, buffer, offset, -1);
      amount_inst.debug = debug;
      shift_inst.debug = debug;
      add.debug = debug;
      if (!tbuf_insert(f, call_at, &amount_inst) ||
          !tbuf_insert(f, call_at + 1, &shift_inst) ||
          !tbuf_insert(f, call_at + 2, &add))
        return false;
      call_at += 3;
    }

    nyr_inst_t *slot = &f->data[call_at];
    nyr_inst_payload_free(slot);
    nyr_inst_t repl = {0};
    repl.op = store ? NYIR_STORE_I64 : NYIR_LOAD_I64;
    repl.dst = store ? -1 : dst;
    repl.a = address;
    repl.b = -1;
    repl.c = store ? value : -1;
    repl.d = -1;
    repl.e = -1;
    repl.f = -1;
    repl.imm = 0;
    repl.cmp = NYIR_CMP_EQ;
    repl.flags = mem_flags;
    repl.scalar_class = scalar;
    repl.debug = debug;
    repl.effects = nyr_inst_effects(&repl);
    *slot = repl;

    if (store && dst >= 0) {
      nyr_inst_t copy = tbuf_make(NYIR_COPY, dst, buffer, -1, -1);
      copy.debug = debug;
      if (!tbuf_insert(f, call_at + 1, &copy))
        return false;
    }
  }
  return true;
}
