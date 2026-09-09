/*
 * Regression driver for native-tbuf-only software prefetch insertion.
 */
#include "util.h"

typedef enum {
  TBUF_PAYLOAD,
  TBUF_DUPLICATE,
  ORDINARY_POINTER,
  UNKNOWN_LOAD,
} address_root_t;

static int emit_tbuf_new(nyir_func_t *f, int count, int width) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_CALL, .dst = -1,
                                    .a = count, .b = width, .c = -1,
                                    .d = -1, .e = -1, .f = -1,
                                    .symbol = "rt_tbuf_new_raw", .imm = 2});
}

static int emit_alloca(nyir_func_t *f) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_ALLOCA, .dst = -1,
                                    .a = -1, .b = -1, .c = -1, .d = -1,
                                    .e = -1, .f = -1, .imm = 1024});
}

static void build_load_loop(nyir_func_t *f, address_root_t root) {
  ny_begin(f);
  int trip = ny_const(f, 64);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  int base = -1;
  if (root == TBUF_PAYLOAD || root == TBUF_DUPLICATE) {
    int width = ny_const(f, 8);
    base = emit_tbuf_new(f, trip, width);
  } else if (root == ORDINARY_POINTER) {
    base = emit_alloca(f);
  } else {
    int backing = emit_alloca(f);
    base = nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_I64, .dst = -1,
                                      .a = backing, .b = -1, .c = -1,
                                      .d = -1, .e = -1, .f = -1});
  }

  ny_br(f, 10);
  ny_label(f, 10);
  int iv = f->next_value;
  ny_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int cond = ny_cmp(f, iv, trip, NYIR_CMP_LT);
  ny_br_if(f, cond, 11);
  ny_label(f, 30);
  ny_ret(f, zero);
  ny_label(f, 11);
  if (root == TBUF_PAYLOAD || root == TBUF_DUPLICATE) {
    base = nyir_emit(f, (nyir_inst_t){.op = NYIR_COPY, .dst = -1,
                                      .a = base, .b = -1, .c = -1,
                                      .d = -1, .e = -1, .f = -1});
  }
  int address = ny_binop(f, NYIR_ADD_I64, base, iv);
  int load_count = root == TBUF_DUPLICATE ? 3 : 1;
  for (int i = 0; i < load_count; ++i) {
    nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_I64, .dst = -1,
                               .a = address, .b = -1, .c = -1, .d = -1,
                               .e = -1, .f = -1});
  }
  int iv_next = ny_binop(f, NYIR_ADD_I64, iv, one);
  ny_br(f, 10);

  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_PHI)
      f->data[i].phi_incoming[1] =
          (nyir_phi_incoming_t){.predecessor_label = 11, .value = iv_next};
  }
}

static int prefetch_count(const nyir_func_t *f);

static void build_many_load_loops(nyir_func_t *f, int loop_count) {
  ny_begin(f);
  int trip = ny_const(f, 64);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  int width = ny_const(f, 8);
  int base = emit_tbuf_new(f, trip, width);
  int phi_indices[17];
  int iv_next_values[17];
  for (int li = 0; li < loop_count; ++li) {
    int header = 100 + li * 3;
    int body = header + 1;
    int exit = header + 2;
    if (li == 0)
      ny_br(f, header);
    ny_label(f, header);
    int iv = f->next_value;
    phi_indices[li] = (int)f->len;
    ny_phi(f, li == 0 ? -1 : header - 1, zero, 0, zero);
    int cond = ny_cmp(f, iv, trip, NYIR_CMP_LT);
    ny_br_if(f, cond, body);
    ny_label(f, exit);
    if (li + 1 == loop_count)
      ny_ret(f, zero);
    else
      ny_br(f, header + 3);
    ny_label(f, body);
    int loop_base = nyir_emit(f, (nyir_inst_t){.op = NYIR_COPY, .dst = -1,
                                                .a = base, .b = -1, .c = -1,
                                                .d = -1, .e = -1, .f = -1});
    int address = ny_binop(f, NYIR_ADD_I64, loop_base, iv);
    nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_I64, .dst = -1,
                               .a = address, .b = -1, .c = -1, .d = -1,
                               .e = -1, .f = -1});
    iv_next_values[li] = ny_binop(f, NYIR_ADD_I64, iv, one);
    ny_br(f, header);
  }
  for (int li = 0; li < loop_count; ++li)
    f->data[phi_indices[li]].phi_incoming[1] =
        (nyir_phi_incoming_t){.predecessor_label = 101 + li * 3,
                              .value = iv_next_values[li]};
}

static bool check_many_loops(void) {
  nyir_func_t f;
  build_many_load_loops(&f, 17);
  bool ok = ny_verify(&f, "prefetch/many-loops") &&
            nyir_prefetch_insert(&f) &&
            ny_verify(&f, "prefetch/many-loops") &&
            prefetch_count(&f) == 17;
  if (!ok)
    fprintf(stderr, "[prefetch/many-loops] prefetches=%d want=17\n",
            prefetch_count(&f));
  nyir_func_free(&f);
  return ok;
}

static int prefetch_count(const nyir_func_t *f) {
  int count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL && in->symbol &&
        strcmp(in->symbol, "__simmd_prefetch") == 0)
      count++;
  }
  return count;
}

static bool check_case(const char *name, address_root_t root, int expected) {
  nyir_func_t f;
  build_load_loop(&f, root);
  if (!ny_verify(&f, name)) {
    nyir_func_free(&f);
    return false;
  }
  if (!nyir_prefetch_insert(&f)) {
    fprintf(stderr, "[%s] prefetch insertion failed\n", name);
    nyir_func_free(&f);
    return false;
  }
  if (!ny_verify(&f, name)) {
    nyir_func_free(&f);
    return false;
  }
  int actual = prefetch_count(&f);
  nyir_func_free(&f);
  if (actual == expected)
    return true;
  fprintf(stderr, "[%s] prefetches=%d want=%d\n", name, actual, expected);
  return false;
}

int main(void) {
  if (!check_case("prefetch/tbuf", TBUF_PAYLOAD, 1) ||
      !check_case("prefetch/tbuf-duplicates", TBUF_DUPLICATE, 1) ||
      !check_case("prefetch/ordinary", ORDINARY_POINTER, 0) ||
      !check_case("prefetch/unknown-load", UNKNOWN_LOAD, 0) ||
      !check_many_loops())
    return 1;
  puts("[prefetch-insert] ok");
  return 0;
}
