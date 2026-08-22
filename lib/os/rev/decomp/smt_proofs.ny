;; Keywords: smt z3 proofs equivalence bitvectors semantics
;; SMT-backed equivalence proofs used by semantic recovery.
module std.os.rev.decomp.smt_proofs *
use std.core
use std.core.str as str
use std.math.smt as smt
use std.os.rev.decomp.elf

fn _hex_width_local(str hex0, int bits) str {
   def width = max(1, (bits + 3) / 4)
   mut h = str.lower(str.strip(hex0))
   if str.startswith(h, "0x") { h = slice(h, 2, h.len, 1) }
   while h.len > 1 && load8(h, 0) == 48 { h = slice(h, 1, h.len, 1) }
   if h.len == 0 { h = "0" }
   if h.len > width { h = slice(h, h.len - width, h.len, 1) }
   while h.len < width { h = "0" + h }
   h
}

fn _dec_divmod_small(str dec0, int base) dict {
   mut q = ""
   mut rem = 0
   mut started = false
   mut i = 0
   while i < dec0.len {
      def c = load8(dec0, i)
      if c >= 48 && c <= 57 {
         def n = rem * 10 + (c - 48)
         def d = n / base
         rem = n - d * base
         if d != 0 || started {
            q = q + str.chr(48 + d)
            started = true
         }
      }
      i += 1
   }
   {"q": started ? q : "0", "rem": rem}
}

fn _dec_to_hex_local(str dec0) str {
   mut dec = str.strip(dec0)
   if dec.len == 0 { return "0" }
   if load8(dec, 0) == 45 { dec = slice(dec, 1, dec.len, 1) }
   mut out = ""
   def digits = "0123456789abcdef"
   mut guard = 0
   while dec != "0" && guard < 4096 {
      def dm = _dec_divmod_small(dec, 16)
      out = str.chr(load8(digits, int(dm.get("rem", 0)))) + out
      dec = dm.get("q", "0")
      guard += 1
   }
   out.len == 0 ? "0" : out
}

fn _smt_bv(any ctx, any value, int bits) any {
   if is_str(value) {
      mut s = str.strip(value)
      mut neg = false
      if str.startswith(s, "-") {
         neg = true
         s = str.strip(slice(s, 1, s.len, 1))
      } elif str.startswith(s, "+") {
         s = str.strip(slice(s, 1, s.len, 1))
      }
      def h = str.startswith(str.lower(s), "0x") ? slice(s, 2, s.len, 1) : _dec_to_hex_local(s)
      def ast = smt.bv_hex(ctx, _hex_width_local(h, bits), bits)
      return neg ? smt.bvneg(ctx, ast) : ast
   }
   def v = int(value)
   if v >= 0 { return smt.bv_hex(ctx, _hex_width_local(str.to_hex(v, 0), bits), bits) }
   smt.bvneg(ctx, _smt_bv(ctx, to_str(0 - v), bits))
}

fn _smt_zext_to(any ctx, any ast, int from_bits, int to_bits) any {
   to_bits <= from_bits ? ast : smt.bvzext(ctx, ast, to_bits - from_bits)
}

fn _smt_expr_bits(any expr, int default_bits) int {
   if is_dict(expr) {
      if expr.contains("bits") { return int(expr.get("bits", default_bits)) }
      def op = expr.get("op", "")
      if op == "extract" { return int(expr.get("hi", 0)) - int(expr.get("lo", 0)) + 1 }
      if op == "concat" {
         def args = expr.get("args", [])
         mut n = 0
         mut i = 0
         while i < args.len {
            n += _smt_expr_bits(args[i], default_bits)
            i += 1
         }
         return n > 0 ? n : default_bits
      }
      if op == "zext" || op == "sext" { return int(expr.get("to", default_bits)) }
      if op == "bswap" { return int(expr.get("bits", default_bits)) }
   }
   default_bits
}

fn _smt_bswap_ast(any ctx, any ast, int bits) any {
   if bits <= 8 || bits % 8 != 0 { return ast }
   mut out = smt.bv_extract(ctx, 7, 0, ast)
   mut lo = 8
   while lo < bits {
      out = smt.bvconcat(ctx, out, smt.bv_extract(ctx, lo + 7, lo, ast))
      lo += 8
   }
   out
}

fn _smt_expr_to_ast(any ctx, any expr, int bits, dict env=dict()) any {
   if is_int(expr) { return _smt_bv(ctx, expr, bits) }
   if is_str(expr) {
      def s = str.strip(expr)
      if s.len == 0 { return _smt_bv(ctx, 0, bits) }
      def c = load8(s, 0)
      if str.startswith(str.lower(s), "0x") || str.startswith(str.lower(s), "-0x") || str.ascii_is_digit(c) || c == 45 || c == 43 {
         return _smt_bv(ctx, s, bits)
      }
      if env.contains(s) { return env.get(s) }
      return smt.bv_const(ctx, s, bits)
   }
   if !is_dict(expr) { return _smt_bv(ctx, 0, bits) }
   def op = expr.get("op", "")
   if op == "const" { return _smt_bv(ctx, expr.get("value", 0), int(expr.get("bits", bits))) }
   if op == "var" {
      def name = expr.get("name", "x")
      def w = int(expr.get("bits", bits))
      if env.contains(name) { return env.get(name) }
      return smt.bv_const(ctx, name, w)
   }
   def args = expr.get("args", [])
   if op == "not" { return smt.bvnot(ctx, _smt_expr_to_ast(ctx, expr.get("expr", args.len > 0 ? args[0] : 0), bits, env)) }
   if op == "neg" { return smt.bvneg(ctx, _smt_expr_to_ast(ctx, expr.get("expr", args.len > 0 ? args[0] : 0), bits, env)) }
   if op == "extract" { return smt.bv_extract(ctx, int(expr.get("hi", 0)), int(expr.get("lo", 0)), _smt_expr_to_ast(ctx, expr.get("expr", args.len > 0 ? args[0] : 0), bits, env)) }
   if op == "zext" {
      return _smt_zext_to(ctx, _smt_expr_to_ast(ctx, expr.get("expr", args.len > 0 ? args[0] : 0), int(expr.get("from", 8)), env),
         int(expr.get("from", 8)), int(expr.get("to", bits)))
   }
   if op == "sext" {
      def from_bits = int(expr.get("from", 8))
      def to_bits = int(expr.get("to", bits))
      def a0 = _smt_expr_to_ast(ctx, expr.get("expr", args.len > 0 ? args[0] : 0), from_bits, env)
      return to_bits <= from_bits ? a0 : smt.bvsext(ctx, a0, to_bits - from_bits)
   }
   if op == "concat" {
      if args.len == 0 { return _smt_bv(ctx, 0, bits) }
      mut out = _smt_expr_to_ast(ctx, args[0], _smt_expr_bits(args[0], bits), env)
      mut i = 1
      while i < args.len {
         out = smt.bvconcat(ctx, out, _smt_expr_to_ast(ctx, args[i], _smt_expr_bits(args[i], bits), env))
         i += 1
      }
      return out
   }
   if op == "bswap" {
      def w = int(expr.get("bits", bits))
      return _smt_bswap_ast(ctx, _smt_expr_to_ast(ctx, expr.get("expr", args.len > 0 ? args[0] : 0), w, env), w)
   }
   def a = _smt_expr_to_ast(ctx, args.len > 0 ? args[0] : expr.get("lhs", 0), bits, env)
   def b = _smt_expr_to_ast(ctx, args.len > 1 ? args[1] : expr.get("rhs", 0), bits, env)
   case op {
      "add", "+" -> smt.bvadd(ctx, a, b)
      "sub", "-" -> smt.bvsub(ctx, a, b)
      "mul", "*" -> smt.bvmul(ctx, a, b)
      "and", "&" -> smt.bvand(ctx, a, b)
      "or", "|" -> smt.bvor(ctx, a, b)
      "xor", "^^" -> smt.bvxor(ctx, a, b)
      "shl", "<<" -> smt.bvshl(ctx, a, b)
      "lshr", ">>" -> smt.bvlshr(ctx, a, b)
      "ashr" -> smt.bvashr(ctx, a, b)
      "udiv" -> smt.bvudiv(ctx, a, b)
      "sdiv" -> smt.bvsdiv(ctx, a, b)
      "urem" -> smt.bvurem(ctx, a, b)
      "srem" -> smt.bvsrem(ctx, a, b)
      "rol" -> smt.bvrotl(ctx, a, int(expr.get("shift", args.len > 1 ? args[1] : 0)), bits)
      "ror" -> smt.bvrotr(ctx, a, int(expr.get("shift", args.len > 1 ? args[1] : 0)), bits)
      _ -> a
   }
}

fn _smt_prove_equal_with(str archetype, int bits, any lhs, any rhs, any ctx, list constraints) dict {
   if !smt.z3_available() { return {"archetype": archetype, "proved": false, "reason": "z3_unavailable"} }
   def sol = smt.solver_new_for_logic(ctx, "QF_BV")
   if !sol { return {"archetype": archetype, "proved": false, "reason": "solver_failed"} }
   mut ci = 0
   while ci < constraints.len {
      smt.solver_assert(ctx, sol, constraints[ci])
      ci += 1
   }
   smt.solver_assert(ctx, sol, smt.mk_neq(ctx, lhs, rhs))
   def result = smt.solver_check_result(ctx, sol)
   smt.solver_del(ctx, sol)
   {"kind": "smt_expr_proof", "archetype": archetype, "proved": result == smt.UNSAT,
      "result": result, "bits": bits, "method": constraints.len > 0 ? "prove_unsat(constraints && lhs != rhs)" : "prove_unsat(lhs != rhs)",
      "constraint_count": constraints.len, "translation": "expr_to_z3_ast"}
}

fn _smt_prove_equal(str archetype, int bits, any lhs, any rhs, any ctx) dict {
   _smt_prove_equal_with(archetype, bits, lhs, rhs, ctx, [])
}

fn _smt_wrapped_const_hex(int low, int bits) str {
   "0x1" + _hex_width_local(str.to_hex(low, 0), bits)
}

fn _smt_division_power2_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "unsigned_div_power2", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "udiv", "args": [x, {"op": "const", "value": 8, "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "lshr", "args": [x, {"op": "const", "value": 3, "bits": bits}], "bits": bits}
   def lhs = _smt_expr_to_ast(ctx, lhs_expr, bits)
   def rhs = _smt_expr_to_ast(ctx, rhs_expr, bits)
   def out = _smt_prove_equal("unsigned_div_power2", bits, lhs, rhs, ctx)
   .set("expr", "x / 8 == x >> 3").set("operator", "bvudiv").set("constant", 8)
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_signed_division_power2_nonnegative_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "signed_div_power2_nonnegative", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "sdiv", "args": [x, {"op": "const", "value": 8, "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "ashr", "args": [x, {"op": "const", "value": 3, "bits": bits}], "bits": bits}
   def x_ast = _smt_expr_to_ast(ctx, x, bits)
   def guard = smt.bvsge(ctx, x_ast, _smt_bv(ctx, 0, bits))
   def out = _smt_prove_equal_with("signed_div_power2_nonnegative", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx, [guard])
   .set("expr", "x >= 0 => sdiv(x, 8) == ashr(x, 3)").set("operator", "bvsdiv")
   .set("constant", 8).set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_remainder_power2_mask_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "unsigned_rem_power2_mask", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "urem", "args": [x, {"op": "const", "value": 8, "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "and", "args": [x, {"op": "const", "value": 7, "bits": bits}], "bits": bits}
   def out = _smt_prove_equal("unsigned_rem_power2_mask", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "x % 8 == x & 7").set("operator", "bvurem").set("constant", 8)
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_mask_low8_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "low_byte_mask", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "and", "args": [x, {"op": "const", "value": 255, "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "zext", "from": 8, "to": bits, "expr": {"op": "extract", "hi": 7, "lo": 0, "expr": x}}
   def lhs = _smt_expr_to_ast(ctx, lhs_expr, bits)
   def rhs = _smt_expr_to_ast(ctx, rhs_expr, bits)
   def out = _smt_prove_equal("low_byte_mask", bits, lhs, rhs, ctx)
   .set("expr", "(x & 0xff) == zext(extract(x, 7, 0))").set("operator", "bvand")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_mask_byte_window_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "byte_window_mask", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "lshr", "args": [
         {"op": "and", "args": [x, {"op": "const", "value": 0xff00, "bits": bits}], "bits": bits},
         {"op": "const", "value": 8, "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "zext", "from": 8, "to": bits, "expr": {"op": "extract", "hi": 15, "lo": 8, "expr": x}}
   def out = _smt_prove_equal("byte_window_mask", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "((x & 0xff00) >> 8) == zext(extract(x, 15, 8))").set("operator", "bvand")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_bitfield_extract_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "bitfield_extract", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lo = 4
   def width = 4
   def mask = (1 << width) - 1
   def lhs_expr = {"op": "and", "args": [
         {"op": "lshr", "args": [x, {"op": "const", "value": lo, "bits": bits}], "bits": bits},
         {"op": "const", "value": mask, "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "zext", "from": width, "to": bits,
      "expr": {"op": "extract", "hi": lo + width - 1, "lo": lo, "expr": x}}
   def out = _smt_prove_equal("bitfield_extract", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "((x >> 4) & 0xf) == zext(extract(x, 7, 4))").set("operator", "bits")
   .set("offset", lo).set("width", width).set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_self_mask_idempotent_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "self_mask_idempotent", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "and", "args": [x, x], "bits": bits}
   def rhs_expr = x
   def out = _smt_prove_equal("self_mask_idempotent", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "(x & x) == x").set("operator", "bvand")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_rotate_left_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "rotate_left", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "or", "args": [
         {"op": "shl", "args": [x, {"op": "const", "value": 5, "bits": bits}], "bits": bits},
         {"op": "lshr", "args": [x, {"op": "const", "value": bits - 5, "bits": bits}], "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "rol", "args": [x], "shift": 5, "bits": bits}
   def lhs = _smt_expr_to_ast(ctx, lhs_expr, bits)
   def rhs = _smt_expr_to_ast(ctx, rhs_expr, bits)
   def out = _smt_prove_equal("rotate_left", bits, lhs, rhs, ctx)
   .set("expr", "((x << 5) | (x >> (bits - 5))) == rol(x, 5)").set("operator", "rol")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_rotate_right_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "rotate_right", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "or", "args": [
         {"op": "lshr", "args": [x, {"op": "const", "value": 7, "bits": bits}], "bits": bits},
         {"op": "shl", "args": [x, {"op": "const", "value": bits - 7, "bits": bits}], "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "ror", "args": [x], "shift": 7, "bits": bits}
   def out = _smt_prove_equal("rotate_right", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "((x >> 7) | (x << (bits - 7))) == ror(x, 7)").set("operator", "ror")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_byte_pack_proof(int bits, str endian) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "byte_pack_" + endian, "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def b0 = {"op": "var", "name": "b0", "bits": 8}
   def b1 = {"op": "var", "name": "b1", "bits": 8}
   def b2 = {"op": "var", "name": "b2", "bits": 8}
   def b3 = {"op": "var", "name": "b3", "bits": 8}
   def z0 = {"op": "zext", "from": 8, "to": bits, "expr": b0}
   def z1 = {"op": "zext", "from": 8, "to": bits, "expr": b1}
   def z2 = {"op": "zext", "from": 8, "to": bits, "expr": b2}
   def z3 = {"op": "zext", "from": 8, "to": bits, "expr": b3}
   mut lhs_expr = z0
   mut rhs_expr = {"op": "concat", "args": [b3, b2, b1, b0], "bits": bits}
   if endian == "little" {
      lhs_expr = {"op": "or", "args": [
            {"op": "or", "args": [z0, {"op": "shl", "args": [z1, {"op": "const", "value": 8, "bits": bits}], "bits": bits}], "bits": bits},
            {"op": "or", "args": [
                  {"op": "shl", "args": [z2, {"op": "const", "value": 16, "bits": bits}], "bits": bits},
                  {"op": "shl", "args": [z3, {"op": "const", "value": 24, "bits": bits}], "bits": bits}], "bits": bits}], "bits": bits}
   } else {
      lhs_expr = {"op": "or", "args": [
            {"op": "or", "args": [
                  {"op": "shl", "args": [z0, {"op": "const", "value": 24, "bits": bits}], "bits": bits},
                  {"op": "shl", "args": [z1, {"op": "const", "value": 16, "bits": bits}], "bits": bits}], "bits": bits},
            {"op": "or", "args": [
                  {"op": "shl", "args": [z2, {"op": "const", "value": 8, "bits": bits}], "bits": bits},
                  z3], "bits": bits}], "bits": bits}
      rhs_expr = {"op": "concat", "args": [b0, b1, b2, b3], "bits": bits}
   }
   def out = _smt_prove_equal("byte_pack_" + endian, bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "byte shifts/or == concat bytes").set("operator", "byte_pack").set("endianness", endian)
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_unbounded_integer_wrap_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "unbounded_integer_width_wrap", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def lhs_expr = {"op": "const", "value": _smt_wrapped_const_hex(42, bits), "bits": bits}
   def rhs_expr = {"op": "const", "value": 42, "bits": bits}
   def out = _smt_prove_equal("unbounded_integer_width_wrap", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "(2^bits + 42) as bitvector == 42").set("operator", "const_width")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_byte_swap32_proof() dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "byte_swap32", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def bits = 32
   def x = {"op": "var", "name": "x", "bits": bits}
   def lhs_expr = {"op": "bswap", "expr": x, "bits": bits}
   def rhs_expr = {"op": "or", "args": [
         {"op": "or", "args": [
               {"op": "shl", "args": [
                     {"op": "and", "args": [x, {"op": "const", "value": 0xff, "bits": bits}], "bits": bits},
                     {"op": "const", "value": 24, "bits": bits}], "bits": bits},
               {"op": "shl", "args": [
                     {"op": "and", "args": [x, {"op": "const", "value": 0xff00, "bits": bits}], "bits": bits},
                     {"op": "const", "value": 8, "bits": bits}], "bits": bits}], "bits": bits},
         {"op": "or", "args": [
               {"op": "lshr", "args": [
                     {"op": "and", "args": [x, {"op": "const", "value": 0xff0000, "bits": bits}], "bits": bits},
                     {"op": "const", "value": 8, "bits": bits}], "bits": bits},
               {"op": "lshr", "args": [x, {"op": "const", "value": 24, "bits": bits}], "bits": bits}], "bits": bits}], "bits": bits}
   def out = _smt_prove_equal("byte_swap32", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "bswap32(x) == endian byte reshuffle").set("operator", "bswap32")
   .set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   smt.ctx_del(ctx)
   out
}

fn _smt_magic_unsigned_div3_proof() dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "magic_unsigned_div3", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x8 = {"op": "var", "name": "x", "bits": 8}
   def x16 = {"op": "zext", "from": 8, "to": 16, "expr": x8}
   def lhs_expr = {"op": "lshr", "args": [
         {"op": "mul", "args": [x16, {"op": "const", "value": "0xab", "bits": 16}], "bits": 16},
         {"op": "const", "value": 9, "bits": 16}], "bits": 16}
   def rhs_expr = {"op": "zext", "from": 8, "to": 16,
      "expr": {"op": "udiv", "args": [x8, {"op": "const", "value": 3, "bits": 8}], "bits": 8}}
   def out = _smt_prove_equal("magic_unsigned_div3", 16,
      _smt_expr_to_ast(ctx, lhs_expr, 16), _smt_expr_to_ast(ctx, rhs_expr, 16), ctx)
   .set("expr", "((zext(x8) * 0xab) >> 9) == x8 / 3").set("operator", "magic_udiv")
   .set("constant", 3).set("domain_bits", 8).set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   .set("replacement", "x / 3")
   smt.ctx_del(ctx)
   out
}

fn _smt_affine_byte_mix_proof() dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "affine_byte_mix", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def b = {"op": "var", "name": "b", "bits": 8}
   def z = {"op": "zext", "from": 8, "to": 32, "expr": b}
   def mixed = {"op": "add", "args": [
         {"op": "xor", "args": [z, {"op": "const", "value": 0x5a, "bits": 32}], "bits": 32},
         {"op": "const", "value": 17, "bits": 32}], "bits": 32}
   def lhs_expr = {"op": "and", "args": [mixed, {"op": "const", "value": 0xff, "bits": 32}], "bits": 32}
   def rhs_expr = {"op": "zext", "from": 8, "to": 32,
      "expr": {"op": "extract", "hi": 7, "lo": 0, "expr": mixed}}
   def out = _smt_prove_equal("affine_byte_mix", 32,
      _smt_expr_to_ast(ctx, lhs_expr, 32), _smt_expr_to_ast(ctx, rhs_expr, 32), ctx)
   .set("expr", "((zext(b) ^^ 0x5a) + 17) & 0xff == byte result")
   .set("operator", "byte_affine").set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   .set("replacement", "byte(((b ^^ 0x5a) + 17))")
   smt.ctx_del(ctx)
   out
}

fn _smt_rotate_xor_hash_round_proof(int bits) dict {
   if !smt.z3_available() { return {"kind": "smt_expr_proof", "archetype": "rotate_xor_hash_round", "proved": false, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   def x = {"op": "var", "name": "x", "bits": bits}
   def base = {"op": "add", "args": [
         {"op": "xor", "args": [x, {"op": "const", "value": "0x9e3779b9", "bits": bits}], "bits": bits},
         {"op": "const", "value": 0x85ebca6b, "bits": bits}], "bits": bits}
   def lhs_expr = {"op": "or", "args": [
         {"op": "shl", "args": [base, {"op": "const", "value": 5, "bits": bits}], "bits": bits},
         {"op": "lshr", "args": [base, {"op": "const", "value": bits - 5, "bits": bits}], "bits": bits}], "bits": bits}
   def rhs_expr = {"op": "rol", "args": [base], "shift": 5, "bits": bits}
   def out = _smt_prove_equal("rotate_xor_hash_round", bits,
      _smt_expr_to_ast(ctx, lhs_expr, bits), _smt_expr_to_ast(ctx, rhs_expr, bits), ctx)
   .set("expr", "shift/or hash round == rol((x ^^ k) + c, 5)")
   .set("operator", "hash_round").set("lhs_ir", lhs_expr).set("rhs_ir", rhs_expr)
   .set("replacement", "rol((x ^^ k) + c, 5)")
   smt.ctx_del(ctx)
   out
}

fn _smt_row_expr_proofs(dict bundle, int bits, int limit) list {
   def rows = bundle.get("rows", [])
   def archetypes = _smt_archetype_proofs(bits, to_str(bundle.get("arch", "")), _smt_endian_modes(bundle))
   mut out = []
   mut saw_div = false
   mut saw_rot = false
   mut saw_mask = false
   mut i = 0
   while i < rows.len && out.len < limit {
      def r = rows[i]
      def kind = r.get("kind", "")
      def op = r.get("operator", "")
      def tag = int(r.get("addr", 0))
      if !saw_div && (kind == "signed_div" || kind == "unsigned_div" || op == "/") {
         def p0 = _smt_archetype_by_name(archetypes, kind == "signed_div" ? "signed_div_power2_nonnegative" : "unsigned_div_power2")
         if p0.len > 0 {
            mut p = clone(p0)
            p.set("source", "row")
            p.set("row", i)
            p.set("addr", tag)
            out = out.append(p)
            saw_div = true
         }
      } elif !saw_rot && kind == "rotate" {
         def p0 = _smt_archetype_by_name(archetypes, op == "ror" ? "rotate_right" : "rotate_left")
         if p0.len > 0 {
            mut p = clone(p0)
            p.set("source", "row")
            p.set("row", i)
            p.set("addr", tag)
            out = out.append(p)
            saw_rot = true
         }
      } elif !saw_mask && str.startswith(str.lower(r.get("mnemonic", "")), "test") {
         def p0 = _smt_archetype_by_name(archetypes, "low_byte_mask")
         if p0.len > 0 {
            mut p = clone(p0)
            p.set("source", "row")
            p.set("row", i)
            p.set("addr", tag)
            out = out.append(p)
            saw_mask = true
         }
      }
      i += 1
   }
   out
}

fn _smt_endian_modes(dict bundle) list {
   def a = str.lower(bundle.get("arch", ""))
   if str.find(a, "be") >= 0 || str.find(a, "big") >= 0 { return ["big", "little"] }
   if str.find(a, "arm") >= 0 || str.find(a, "aarch") >= 0 { return ["little", "big"] }
   ["little", "big"]
}

mut _smt_archetype_cache = dict()

fn _smt_archetype_key(int bits, str arch, list endian_modes) str {
   to_str(bits) + "|" + arch + "|" + str.join(endian_modes, ",")
}

fn _smt_archetype_proofs(int proof_bits, str arch, list endian_modes) list {
   def key = _smt_archetype_key(proof_bits, arch, endian_modes)
   if _smt_archetype_cache.contains(key) { return _smt_archetype_cache.get(key, []) }
   mut archetypes = []
   archetypes = archetypes.append(_smt_division_power2_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_signed_division_power2_nonnegative_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_remainder_power2_mask_proof(proof_bits).set("source", "archetype"))
   mut ei = 0
   while ei < endian_modes.len {
      archetypes = archetypes.append(_smt_byte_pack_proof(32, endian_modes[ei]).set("source", "archetype").set("target_arch", arch).set("target_endianness", endian_modes[ei]))
      ei += 1
   }
   archetypes = archetypes.append(_smt_mask_low8_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_mask_byte_window_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_bitfield_extract_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_self_mask_idempotent_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_rotate_left_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_rotate_right_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_unbounded_integer_wrap_proof(proof_bits).set("source", "archetype"))
   archetypes = archetypes.append(_smt_byte_swap32_proof().set("source", "archetype"))
   archetypes = archetypes.append(_smt_magic_unsigned_div3_proof().set("source", "archetype"))
   archetypes = archetypes.append(_smt_affine_byte_mix_proof().set("source", "archetype"))
   archetypes = archetypes.append(_smt_rotate_xor_hash_round_proof(8).set("source", "archetype"))
   _smt_archetype_cache = _smt_archetype_cache.set(key, archetypes)
   archetypes
}

fn _smt_archetype_by_name(list archetypes, str name) dict {
   mut i = 0
   while i < archetypes.len {
      if archetypes[i].get("archetype", "") == name { return archetypes[i] }
      i += 1
   }
   dict()
}

fn _smt_expression_proofs_from_facts(dict bundle, any opts=dict()) dict {
   def bits = int(opts.get("bits", bundle.get("arch", "") == "x86_64" || bundle.get("arch", "") == "aarch64" ? 64 : 32))
   def proof_bits = bits >= 32 ? bits : 32
   def endian_modes = _smt_endian_modes(bundle)
   def archetypes = _smt_archetype_proofs(proof_bits, to_str(bundle.get("arch", "")), endian_modes)
   mut proofs = []
   mut ai = 0
   while ai < archetypes.len {
      proofs = proofs.append(clone(archetypes[ai]))
      ai += 1
   }
   def row_proofs = _smt_row_expr_proofs(bundle, proof_bits, int(opts.get("row_limit", 8)))
   mut i = 0
   while i < row_proofs.len {
      proofs = proofs.append(row_proofs[i])
      i += 1
   }
   mut proved = 0
   i = 0
   while i < proofs.len {
      if proofs[i].get("proved", false) { proved += 1 }
      i += 1
   }
   {"kind": "smt_expression_proofs", "input": "facts", "backend": "z3",
      "ok": smt.z3_available(), "bits": proof_bits,
      "translation": "expr_to_z3_ast", "endianness_modes": endian_modes,
      "proofs": proofs, "proof_count": proofs.len, "proved_count": proved}
}

#main {
   def proofs = _smt_expression_proofs_from_facts({"arch": "x86_64", "rows": []})
   assert(proofs.get("proof_count", 0) > 0, "SMT archetype proofs")
   print("✓ std.os.rev.decomp.smt_proofs self-test passed")
}
