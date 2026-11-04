/*
 * LICM keeps the address-forming ADD of a loop-owned access inside the loop;
 * later vectorization and native home tracking rely on that loop-local shape.
 * Invariant integer index arithmetic (the SHL) may still be hoisted, while
 * float/vector accesses keep their whole address chain in the loop.
 */
#include "support.h"
#include "code/backend/ir/opt/loop.h"

static bool build_address_loop(nyr_func_t *f, bool float_access) {
  nyr_opt_begin(f);
  int base = nyr_opt_emit_symbol(f, NYIR_ADDR_SYMBOL, "licm_address_base");
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int eight = nyr_opt_const(f, 8);
  int limit = nyr_opt_const(f, 4);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv_phi = (int)f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, 0);
  int acc_phi = (int)f->len;
  int acc = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, 0);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, acc);
  nyr_opt_label(f, 11);
  int offset = nyr_opt_binop(f, NYIR_SHL_I64, eight, one);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
  int load = nyr_opt_emit_abc(f, NYIR_LOAD_I64, address, -1, -1);
  if (float_access)
    f->data[f->len - 1].scalar_class = NYIR_SCALAR_F64;
  int next_acc = nyr_opt_binop(f, NYIR_ADD_I64, acc, load);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  for (size_t i = 0; i < f->len; ++i) {
    if ((int)i == iv_phi || (int)i == acc_phi)
      for (size_t k = 0; k < f->data[i].phi_incoming_len; ++k)
        if (f->data[i].phi_incoming[k].predecessor_label == 11)
          f->data[i].phi_incoming[k].value =
              (int)i == iv_phi ? next_iv : next_acc;
  }
  return nyr_alias_analysis(f) && nyr_opt_verify(f, "licm-address-before");
}

static size_t find_op(const nyr_func_t *f, nyr_op_t op, int dst) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == op && (dst < 0 || f->data[i].dst == dst))
      return i;
  return SIZE_MAX;
}

static size_t find_header(const nyr_func_t *f) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == 10)
      return i;
  return SIZE_MAX;
}

static bool check(bool float_access) {
  nyr_func_t f;
  if (!build_address_loop(&f, float_access)) {
    nyr_ir_func_free(&f);
    return false;
  }
  size_t load = find_op(&f, NYIR_LOAD_I64, -1);
  int address = load != SIZE_MAX ? f.data[load].a : -1;
  bool ok = address >= 0 && nyr_licm(&f) && nyr_opt_verify(&f, "licm-address-after");
  size_t header = find_header(&f);
  load = find_op(&f, NYIR_LOAD_I64, -1);
  size_t add = find_op(&f, NYIR_ADD_I64, address);
  size_t shift = find_op(&f, NYIR_SHL_I64, -1);
  ok = ok && header != SIZE_MAX && load != SIZE_MAX && load > header &&
       add != SIZE_MAX && add > header &&
       (!float_access || (shift != SIZE_MAX && shift > header));
  if (!ok) {
    fprintf(stderr, "[licm-address-component] %s address arithmetic was hoisted\n",
            float_access ? "float" : "integer");
    nyr_dump(stderr, &f, "licm-address-component");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  bool ok = check(false) && check(true);
  if (ok)
    puts("[licm-address-component] ok address-forming arithmetic remains in loop");
  return ok ? 0 : 1;
}
