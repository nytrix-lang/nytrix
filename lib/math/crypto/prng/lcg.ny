;; Keywords: prng lcg math crypto
;; PRNG analysis routines for linear-congruential generator prediction and recovery.
;; Recover multiplier, increment, modulus from output sequence; predict next values.
;; x_{n+1} = a*x_n + c (mod m)
;; Reference:
;; - https://cseweb.ucsd.edu/~mihir/papers/dss-lcg.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap5.pdf
;; References:
;; - std.math.crypto.prng
;; - std.math.crypto
module std.math.crypto.prng.lcg(lcg_next, lcg_previous, lcg_predict_next, lcg_crack_multiplier, lcg_crack_increment, lcg_crack_modulus, lcg_crack_full, lcg_recover_state_mod_outputs, lcg_smt_recover_seed_from_bit_outputs, msvc_rand_next_state, msvc_rand_output, msvc_rand_outputs, msvc_rand_key_bytes, msvc_rand_key, msvc_rand_crypt, msvc_rand_bruteforce_seed, tlcg_modulus, tlcg_next_state, tlcg_high_output, tlcg_output_at, tlcg_recover, tlcg_recover_state)
use std.math.nt
use std.math.bin
use std.math.smt
use std.math.crypto.lattice.cvp as lcvp

fn _z(any x) any { is_bigint(x) ? x : Z(x) }

fn _byte_len(any data) int {
   if is_str(data) || is_bytes(data) { return len(data) }
   data.len
}

fn _byte_at(any data, int i) int {
   if is_str(data) || is_bytes(data) { return load8(data, i) & 255 }
   int(data[i]) & 255
}

fn _smt_unsat_result() dict { dict().set("sat", false) }

fn lcg_next(any x, any a, any c, any m) any {
   "Compute one LCG step: returns(a*x + c) mod m."
   (a * x + c) % m
}

fn lcg_previous(any x, any a, any c, any m) any {
   "Invert one LCG step when a is invertible modulo m: returns a^-1*(x-c) mod m, or 0 if not invertible."
   if gcd(a, m) != 1 { return 0 }
   def inv_a = inverse_mod(a, m)
   ((x - c) % m + m) % m * inv_a % m
}

fn lcg_predict_next(list outputs, any a, any c, any m) any {
   "Predict the next LCG output given a list of previous outputs and known parameters.
   Returns(a * last + c) mod m."
   def n = outputs.len
   n == 0 ? 0 : lcg_next(outputs.get(n - 1), a, c, m)
}

fn lcg_crack_multiplier(list outputs, any m) any {
   "Recover LCG multiplier a from three consecutive outputs and known modulus m.
   Uses: a = (y2-y1) * (y1-y0)^-1 mod m.
   Returns a, or 0 if denominator is not invertible or not enough data."
   if outputs.len < 3 { return 0 }
   def y0, y1 = outputs.get(0), outputs.get(1)
   def y2 = outputs.get(2)
   def den = ((y1 - y0) % m + m) % m
   if gcd(den, m) != 1 { return 0 }
   ((y2 - y1) % m + m) % m * inverse_mod(den, m) % m
}

fn lcg_crack_increment(list outputs, any a, any m) any {
   "Recover LCG increment c from two consecutive outputs and known a, m.
   Uses: c = y1 - a*y0 mod m.
   Returns c, or 0 if not enough data."
   if outputs.len < 2 { return 0 }
   def y0, y1 = outputs.get(0), outputs.get(1)
   ((y1 - a * y0) % m + m) % m
}

fn lcg_crack_modulus(list outputs) any {
   "Recover LCG modulus m from a sequence of at least 4 consecutive outputs.
   Method: compute second differences t_i = y_{i+1}-y_i, then m | t_{i+1}*t_{i-1}-t_i^2.
   Take GCD of multiple such values. Returns m or 0 on failure."
   def n = outputs.len
   if n < 4 { return 0 }
   def t0, t1 = outputs.get(1) - outputs.get(0), outputs.get(2) - outputs.get(1)
   def t2 = outputs.get(3) - outputs.get(2)
   mut cand = t1 * t1 - t0 * t2
   if cand < 0 { cand = 0 - cand }
   if cand == 0 { return 0 }
   if n >= 5 {
      def t3 = outputs.get(4) - outputs.get(3)
      mut c2 = t2 * t2 - t1 * t3
      if c2 < 0 { c2 = 0 - c2 }
      cand = gcd(cand, c2)
   }
   cand
}

fn lcg_crack_full(list outputs) list {
   "Recover all LCG parameters [a, c, m] from at least 4 consecutive raw outputs.
   Uses GCD-based modulus recovery, then derives a and c.
   Returns [a, c, m] or [0, 0, 0] on failure."
   def m = lcg_crack_modulus(outputs)
   if m == 0 { return [0, 0, 0] }
   def a, c = lcg_crack_multiplier(outputs, m), lcg_crack_increment(outputs, a, m)
   [a, c, m]
}

fn _lcg_state_matches_mod_outputs(any state, list outputs_mod, any a, any c, any m, any output_mod) bool {
   mut cur = _z(state)
   mut i = 0
   while i < outputs_mod.len {
      if mod(cur, output_mod) != mod(_z(outputs_mod[i]), output_mod) { return false }
      if i + 1 < outputs_mod.len { cur = lcg_next(cur, a, c, m) }
      i += 1
   }
   true
}

fn _lcg_recover_state_mod_outputs_pruned(list outputs_mod, any a, any c, any m, any output_mod) any {
   def om, mm = _z(output_mod), _z(m)
   def aa, cc = _z(a), _z(c)
   if outputs_mod.len < 2 || aa <= Z(0) { return Z(-1) }
   def r0, r1 = mod(_z(outputs_mod[0]), om), mod(_z(outputs_mod[1]), om)
   def t_max = (mm - Z(1) - r0) / om
   if t_max < Z(0) { return Z(-1) }
   def y0 = aa * r0 + cc
   def step = aa * om
   def q_min = y0 / mm
   def q_max = (y0 + step * t_max) / mm
   mut q = q_min
   while q <= q_max {
      if mod(y0 - q * mm - r1, om) == Z(0) {
         def lo_num = q * mm - y0
         mut lo = Z(0)
         if lo_num > Z(0) { lo = (lo_num + step - Z(1)) / step }
         def hi_num = (q + Z(1)) * mm - Z(1) - y0
         if hi_num >= Z(0) {
            mut hi = hi_num / step
            if hi > t_max { hi = t_max }
            mut t = lo
            while t <= hi {
               def state = r0 + om * t
               if _lcg_state_matches_mod_outputs(state, outputs_mod, aa, cc, mm, om) { return state }
               t += Z(1)
            }
         }
      }
      q += Z(1)
   }
   Z(-1)
}

fn lcg_recover_state_mod_outputs(list outputs_mod, any a, any c, any m, any output_mod) any {
   "Recover the first raw LCG state when only `state mod output_mod` is observed.
   Parameters `a`, `c`, and `m` are known. Returns the first matching state or -1."
   if outputs_mod.len == 0 || output_mod <= 0 || m <= 0 { return Z(-1) }
   if outputs_mod.len >= 2 {
      def pruned = _lcg_recover_state_mod_outputs_pruned(outputs_mod, a, c, m, output_mod)
      if pruned != Z(-1) { return pruned }
   }
   def om, mm = _z(output_mod), _z(m)
   mut state = mod(_z(outputs_mod[0]), om)
   while state < mm {
      if _lcg_state_matches_mod_outputs(state, outputs_mod, a, c, m, om) { return state }
      state += om
   }
   Z(-1)
}

fn lcg_smt_recover_seed_from_bit_outputs(list bits, any a, any c, int modulus_bits, int bit_index) dict {
   "Recover an LCG seed from one leaked bit of each successive state.
   Models `x = a*x + c mod 2^modulus_bits`, then constrains
   `((x >> bit_index) & 1)` for each observed bit. The first bit constrains the
   first state after one LCG step. Returns dict `{sat, seed}` ; `seed` is the
   pre-output state when satisfiable."
   if bits.len == 0 || modulus_bits <= 0 || modulus_bits > 63 { return _smt_unsat_result() }
   if bit_index < 0 || bit_index >= modulus_bits { return _smt_unsat_result() }
   if !z3_available() { return _smt_unsat_result() }
   def ctx = z3_ctx_new()
   if !ctx { return _smt_unsat_result() }
   def solver = z3_solver_new(ctx)
   if !solver {
      z3_ctx_del(ctx)
      return _smt_unsat_result()
   }
   def width = modulus_bits
   def seed = z3_bv_const(ctx, "seed", width)
   mut state = seed
   mut i = 0
   while i < bits.len {
      state = z3_bvadd(ctx, z3_bvmul(ctx, z3_bv_u64(ctx, a, width), state), z3_bv_u64(ctx, c, width))
      def got = z3_bvextract(ctx, bit_index, bit_index, state)
      def want = z3_bv_u64(ctx, int(bits[i]) & 1, 1)
      z3_solver_assert(ctx, solver, z3_eq(ctx, got, want))
      i += 1
   }
   def sat = z3_solver_check(ctx, solver)
   mut result = dict().set("sat", sat)
   if sat {
      def model_seed = z3_model_eval_u64(ctx, solver, seed)
      result = result.set("seed", model_seed == nil ? Z(-1) : Z(model_seed))
   }
   z3_solver_del(ctx, solver)
   z3_ctx_del(ctx)
   result
}

fn msvc_rand_next_state(any state) any {
   "Advance the MSVC/Visual C `rand()` state: `state = state*214013 + 2531011 mod 2^32`."
   ((int(state) & 4294967295) * 214013 + 2531011) & 4294967295
}

fn msvc_rand_output(any state) int {
   "Return the 15-bit output value produced from an already-advanced MSVC rand state."
   (int(state) >> 16) & 32767
}

fn msvc_rand_outputs(any seed, int count) list {
   "Generate `count` MSVC rand outputs from `seed`."
   mut state = _z(seed)
   mut out = list(count)
   __list_set_len(out, count)
   mut i = 0
   while i < count {
      state = msvc_rand_next_state(state)
      __store_item_fast(out, i, msvc_rand_output(state))
      i += 1
   }
   out
}

fn msvc_rand_key_bytes(any seed, int key_len=32, str charset="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789") list<int> {
   "Generate key bytes with MSVC rand outputs reduced modulo `charset.len`.
   This matches code shaped like `charset[Rand() % (sizeof(charset)-1)]`."
   if key_len <= 0 || charset.len == 0 { return [] }
   mut state = _z(seed)
   mut out = list(key_len)
   __list_set_len(out, key_len)
   mut i = 0
   while i < key_len {
      state = msvc_rand_next_state(state)
      def idx = msvc_rand_output(state) % charset.len
      __store_item_fast(out, i, load8(charset, idx) & 255)
      i += 1
   }
   out
}

fn msvc_rand_key(any seed, int key_len=32, str charset="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789") str {
   "Generate an ASCII key string with `msvc_rand_key_bytes`."
   msvc_rand_key_bytes(seed, key_len, charset).text
}

fn msvc_rand_crypt(any data, any key) list<int> {
   "XOR `data` with repeating `key`. `data` and `key` may be strings, bytes, or byte lists."
   def n, k = _byte_len(data), _byte_len(key)
   if k == 0 { return [] }
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, _byte_at(data, i) ^^ _byte_at(key, i % k))
      i += 1
   }
   out
}

fn _msvc_rand_seed_matches_checks(list<int> expected_by_ki, int seed, int max_ki, str charset) bool {
   mut state = seed & 4294967295
   def clen = charset.len
   mut ki = 0
   while ki <= max_ki {
      state = ((state * 214013 + 2531011) & 4294967295)
      def expected = expected_by_ki[ki]
      if expected >= 0 && (load8(charset, ((state >> 16) & 32767) % clen) & 255) != expected { return false }
      ki += 1
   }
   true
}

fn _msvc_rand_build_checks(any data, any known_prefix, any known_suffix, int key_len) list {
   def n = _byte_len(data)
   mut list<int> expected = list(key_len)
   __list_set_len(expected, key_len)
   mut b = 0
   while b < key_len {
      __store_item_fast(expected, b, -1)
      b += 1
   }
   mut max_ki = -1
   mut i = 0
   while i < _byte_len(known_prefix) {
      def ki, kb = i % key_len, _byte_at(data, i) ^^ _byte_at(known_prefix, i)
      if expected[ki] >= 0 && expected[ki] != kb { return [expected, max_ki, false] }
      expected[ki] = kb
      if ki > max_ki { max_ki = ki }
      i += 1
   }
   def sn = _byte_len(known_suffix)
   i = 0
   while i < sn {
      def pos = n - sn + i
      def ki = pos % key_len
      def kb = _byte_at(data, pos) ^^ _byte_at(known_suffix, i)
      if expected[ki] >= 0 && expected[ki] != kb { return [expected, max_ki, false] }
      expected[ki] = kb
      if ki > max_ki { max_ki = ki }
      i += 1
   }
   [expected, max_ki, true]
}

fn msvc_rand_bruteforce_seed(any ciphertext, any known_prefix, int start_seed, int end_seed, int key_len=32, str charset="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789", any known_suffix="") any {
   "Search inclusive seed range for an MSVC-rand key that decrypts ciphertext
   to bytes matching known_prefix and optional known_suffix.
   Returns [seed, key, plaintext_bytes], or nil when no candidate matches."
   if end_seed < start_seed { return nil }
   if key_len <= 0 || charset.len == 0 { return nil }
   def n = _byte_len(ciphertext)
   def pn = _byte_len(known_prefix)
   def sn = _byte_len(known_suffix)
   if n < pn || n < sn { return nil }
   def check_info = _msvc_rand_build_checks(ciphertext, known_prefix, known_suffix, key_len)
   def checks = check_info[0]
   def max_ki = check_info[1]
   if !check_info[2] { return nil }
   mut seed = start_seed
   while seed <= end_seed {
      if _msvc_rand_seed_matches_checks(checks, seed, max_ki, charset) {
         def key_bytes = msvc_rand_key_bytes(seed, key_len, charset)
         def key = key_bytes.text
         return [seed, key, msvc_rand_crypt(ciphertext, key_bytes)]
      }
      seed += 1
   }
   nil
}

fn _rhs_from_y(list ys, any a, any b, int i) any { _z(ys[i + 1]) - _z(a) * _z(ys[i]) - _z(b) }

fn tlcg_modulus(int modulus_bits) any {
   "Return modulus `2^modulus_bits`."
   if modulus_bits <= 0 { return Z(0) }
   bigint_lshift(Z(1), modulus_bits)
}

fn tlcg_next_state(any state, any a, any b, any m) any {
   "One LCG step: `(a*state + b) mod m`."
   mod(_z(a) * _z(state) + _z(b), _z(m))
}

fn tlcg_high_output(any state, int low_bits) any {
   "High-part observable output when low `low_bits` bits are truncated."
   _z(state) / bigint_lshift(Z(1), low_bits)
}

fn tlcg_output_at(any state0, any a, any b, int modulus_bits, int low_bits, int idx) any {
   "Return high output at relative index `idx` from recovered state `state0`."
   def m = tlcg_modulus(modulus_bits)
   if m == 0 { return Z(-1) }
   mut state = mod(_z(state0), m)
   if idx >= 0 {
      mut i = 0
      while i < idx {
         state = tlcg_next_state(state, a, b, m)
         i += 1
      }
      return tlcg_high_output(state, low_bits)
   }
   if gcd(_z(a), m) != Z(1) { return Z(-1) }
   def inv_a, inv_b = inverse_mod(_z(a), m), mod(-inv_a * _z(b), m)
   mut i = 0
   while i < -idx {
      state = mod(inv_a * state + inv_b, m)
      i += 1
   }
   tlcg_high_output(state, low_bits)
}

fn _zero_matrix(int rows, int cols) list {
   mut out = []
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < cols {
         row = row.append(Z(0))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _scaled_high_outputs(list outputs_high, any trunc) list {
   mut ys = []
   mut i = 0
   while i < outputs_high.len {
      ys = ys.append(_z(outputs_high[i]) * trunc)
      i += 1
   }
   ys
}

fn _build_recover_matrix(int n, any a, any m) list {
   def dim = 2 * n - 1
   mut M, i = _zero_matrix(dim, dim), 0
   while i < n - 1 {
      mut r0 = M[i]
      r0[i] = _z(a)
      r0[i + n - 1] = Z(1)
      M[i] = r0
      mut r1 = M[i + 1]
      r1[i] = Z(-1)
      M[i + 1] = r1
      mut rk = M[n + i]
      rk[i] = _z(m)
      M[n + i] = rk
      i += 1
   }
   mut rlast = M[n - 1]
   rlast[dim - 1] = Z(1)
   M[n - 1] = rlast
   M
}

fn _build_recover_bounds(list ys, any a, any b, any trunc, int n) list {
   mut lb, ub = [], []
   mut i = 0
   while i < n - 1 {
      def rhs = _rhs_from_y(ys, a, b, i)
      lb, ub = lb.append(rhs), ub.append(rhs)
      i += 1
   }
   i = 0
   while i < n {
      lb, ub = lb.append(Z(0)), ub.append(trunc)
      i += 1
   }
   [lb, ub]
}

fn _extract_zk_from_fin(list fin, int n, int dim) list {
   if fin.len != dim { return [[], []] }
   mut zs, ks = [], []
   mut i = 0
   while i < n {
      zs = zs.append(_z(fin[i]))
      i += 1
   }
   i = 0
   while i < n - 1 {
      ks = ks.append(_z(fin[n + i]))
      i += 1
   }
   [zs, ks]
}

fn _extract_zk_from_vals(list vals, list ys, any a, any b, any m, int n, int dim) list {
   if vals.len != dim { return [[], []] }
   mut zs, ks = [], []
   mut i = 0
   while i < n {
      zs = zs.append(_z(vals[(n - 1) + i]))
      i += 1
   }
   i = 0
   while i < n - 1 {
      def rhs = _rhs_from_y(ys, a, b, i)
      def zi = _z(zs[i])
      def zj = _z(zs[i + 1])
      ks = ks.append((rhs - (_z(a) * zi - zj)) / _z(m))
      i += 1
   }
   [zs, ks]
}

fn _recover_low_parts(list outputs_high, any a, any b, any m, any trunc) list {
   def n = outputs_high.len
   if n < 2 { return [[], []] }
   def ys = _scaled_high_outputs(outputs_high, trunc)
   def dim = 2 * n - 1
   def M = _build_recover_matrix(n, a, m)
   def bounds = _build_recover_bounds(ys, a, b, trunc, n)
   def solved = lcvp.solve_weighted_bounds(M, bounds[0], bounds[1])
   def fin = (is_list(solved) && solved.len > 4) ? solved[4] : []
   if fin.len == dim { return _extract_zk_from_fin(fin, n, dim) }
   def vals = (is_list(solved) && solved.len > 0) ? solved[0] : []
   _extract_zk_from_vals(vals, ys, a, b, m, n, dim)
}

fn tlcg_recover(any outputs_high, any a, any b, int modulus_bits, int low_bits) list {
   "Recover first observed internal state from truncated high-part outputs."
   def n = is_list(outputs_high) ? outputs_high.len : 0
   if n < 2 { return [Z(0), [], []] }
   def m = tlcg_modulus(modulus_bits)
   if m == 0 { return [Z(0), [], []] }
   if low_bits <= 0 || low_bits >= modulus_bits { return [Z(0), [], []] }
   def trunc = bigint_lshift(Z(1), low_bits)
   def parts = _recover_low_parts(outputs_high, a, b, m, trunc)
   def low_parts = parts[0]
   def carries = parts[1]
   if low_parts.len != n { return [Z(0), low_parts, carries] }
   mut state0 = mod(_z(outputs_high[0]) * trunc + _z(low_parts[0]), m)
   mut ok = true
   mut s = state0
   mut i = 0
   while i < n {
      if tlcg_high_output(s, low_bits) != _z(outputs_high[i]) {
         ok = false
         break
      }
      if i + 1 < n { s = tlcg_next_state(s, a, b, m) }
      i += 1
   }
   [ok ? state0 : Z(0), low_parts, carries]
}

fn tlcg_recover_state(any outputs_high, any a, any b, int modulus_bits, int low_bits) any {
   "Recover first observed internal state from truncated outputs, or `0` on failure."
   tlcg_recover(outputs_high, a, b, modulus_bits, low_bits)[0]
}

#main {
   def seed = 18765
   def charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789-"
   def key = msvc_rand_key(seed, 32, charset)
   def msg = "plain-lcg-fixture"
   def ct = msvc_rand_crypt(msg, key)
   def hit = msvc_rand_bruteforce_seed(ct, "plain-", 18000, 19000, 32, charset, "")
   assert(hit != nil && hit[0] == seed && hit[2] == msg.to_bytes, "MSVC rand brute-force roundtrip")
   print("✓ std.math.crypto.prng.lcg self-test passed")
}
