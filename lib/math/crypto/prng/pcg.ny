;; Keywords: prng pcg math crypto
;; PRNG analysis routines for PCG stepping, rewinding, and state advancement.
;; Reference:
;; - https://www.pcg-random.org/paper.html
;; Covers the common pcg32 XSH-RR generator: 64-bit LCG state, 32-bit output.
;; References:
;; - std.math.crypto.prng
;; - std.math.crypto
module std.math.crypto.prng.pcg(pcg32_default_multiplier, pcg32_step, pcg32_output, pcg32_next, pcg32_stream_increment, pcg32_advance, pcg32_rewind, poly_pcg_choose_window, poly_pcg_newton_coeffs, poly_pcg_eval_next, poly_pcg_advance_from_states)
use std.math.nt

fn _PCG64_MOD() bigint { Z("18446744073709551616") }

fn _PCG32_MOD() bigint { Z("4294967296") }

fn _PCG32_MULT() bigint { Z("6364136223846793005") }

fn _u64(any x) bigint { mod(Z(x), _PCG64_MOD()) }

fn _u32(any x) bigint { mod(Z(x), _PCG32_MOD()) }

fn _rotr32(any x, any r) bigint {
   def rr = int(r) & 31
   _u32((_u32(x) >> rr) | (_u32(x) << ((32 - rr) & 31)))
}

fn pcg32_default_multiplier() bigint {
   "Return the standard PCG32 64-bit LCG multiplier."
   _PCG32_MULT()
}

fn pcg32_stream_increment(any stream) bigint {
   "Return the odd PCG increment for a stream id."
   _u64((Z(stream) << Z(1)) | Z(1))
}

fn pcg32_step(any state, any inc) bigint {
   "Advance the internal PCG32 LCG state by one step."
   _u64(Z(state) * _PCG32_MULT() + Z(inc))
}

fn pcg32_output(any state) bigint {
   "Compute the pcg32 XSH-RR output from a pre-advanced/internal state."
   def s = _u64(state)
   def xorshifted = _u32(((s >> Z(18)) ^^ s) >> Z(27))
   def rot = int(s >> Z(59))
   _rotr32(xorshifted, rot)
}

fn pcg32_next(any state, any inc) list {
   "Return [next_state, output] for pcg32."
   def old = _u64(state)
   [pcg32_step(old, inc), pcg32_output(old)]
}

fn pcg32_advance(any state, any inc, any delta) bigint {
   "Advance PCG32 state by delta steps using LCG exponentiation."
   mut cur_mult = _PCG32_MULT()
   mut cur_plus = _u64(inc)
   mut acc_mult = Z(1)
   mut acc_plus = Z(0)
   mut d = Z(delta)
   while d > Z(0) {
      if (d & Z(1)) != Z(0) {
         acc_mult = _u64(acc_mult * cur_mult)
         acc_plus = _u64(acc_plus * cur_mult + cur_plus)
      }
      cur_plus = _u64((cur_mult + Z(1)) * cur_plus)
      cur_mult = _u64(cur_mult * cur_mult)
      d = d >> Z(1)
   }
   _u64(acc_mult * Z(state) + acc_plus)
}

fn pcg32_rewind(any state, any inc, any delta=1) bigint {
   "Rewind PCG32 state by delta steps. Requires odd multiplier modulo 2^64."
   def inv_mult = inverse_mod(_PCG32_MULT(), _PCG64_MOD())
   mut cur = _u64(state)
   mut i = Z(0)
   while i < Z(delta) {
      cur = _u64((cur - Z(inc)) * inv_mult)
      i += Z(1)
   }
   cur
}

fn _pcg_abs(any x) bigint { Z(x) < Z(0) ? Z(0) - Z(x) : Z(x) }

fn _pcg_v2(any x) int {
   "Return the 2-adic valuation of a non-zero integer."
   mut y = _pcg_abs(x)
   if y == Z(0) { return 1 << 30 }
   mut n = 0
   while (y & Z(1)) == Z(0) {
      y = y >> Z(1)
      n += 1
   }
   n
}

fn _rat_new(any num, any den) list {
   mut n, d = Z(num), Z(den)
   assert(d != Z(0), "rational denominator is non-zero")
   if d < Z(0) { n = Z(0) - n d = Z(0) - d }
   def g = gcd(_pcg_abs(n), d)
   [n / g, d / g]
}

fn _rat_sub(list a, list b) list {
   _rat_new(Z(a[0]) * Z(b[1]) - Z(b[0]) * Z(a[1]), Z(a[1]) * Z(b[1]))
}

fn _rat_add(list a, list b) list {
   _rat_new(Z(a[0]) * Z(b[1]) + Z(b[0]) * Z(a[1]), Z(a[1]) * Z(b[1]))
}

fn _rat_mul_int(list a, any k) list {
   _rat_new(Z(a[0]) * Z(k), Z(a[1]))
}

fn _rat_div_int(list a, any k) list {
   _rat_new(Z(a[0]), Z(a[1]) * Z(k))
}

fn _rat_to_mod(list a, any modulus) bigint {
   "Map an exact rational to Z/modulus. Powers of two in the denominator must cancel."
   mut n, d = Z(a[0]), Z(a[1])
   def m = Z(modulus)
   while (d & Z(1)) == Z(0) {
      assert((n & Z(1)) == Z(0), "rational denominator is not invertible modulo target")
      n = n >> Z(1)
      d = d >> Z(1)
   }
   mod(mod(n, m) * inverse_mod(mod(d, m), m), m)
}

fn poly_pcg_choose_window(list states, any modulus, int min_window=16) int {
   "Choose a recent-state interpolation window for a polynomial congruential generator modulo a power of two."
   if states.len <= 1 { return 0 }
   def target = Z(states[states.len - 1])
   def need = max(1, bit_length(Z(modulus)) - 1)
   mut total, used = 0, 0
   mut i = states.len - 2
   while i >= 0 {
      total += _pcg_v2(target - Z(states[i]))
      used += 1
      if used >= min_window && total >= need { return used }
      i -= 1
   }
   max(1, min(states.len - 1, min_window))
}

fn poly_pcg_newton_coeffs(list xs, list ys) list {
   "Return exact Newton divided-difference coefficients for y=f(x)."
   assert(xs.len == ys.len && xs.len > 0, "matching non-empty interpolation points")
   mut coeffs = []
   mut i = 0
   while i < ys.len {
      coeffs = coeffs.append(_rat_new(ys[i], 1))
      i += 1
   }
   mut order = 1
   while order < xs.len {
      i = xs.len - 1
      while i >= order {
         coeffs[i] = _rat_div_int(_rat_sub(coeffs[i], coeffs[i - 1]), Z(xs[i]) - Z(xs[i - order]))
         i -= 1
      }
      order += 1
   }
   coeffs
}

fn poly_pcg_eval_next(any state, list xs, list coeffs, any modulus) bigint {
   "Evaluate a Newton-form polynomial transition at state modulo modulus."
   assert(xs.len == coeffs.len && xs.len > 0, "matching interpolation basis")
   def x = Z(state)
   mut acc = coeffs[coeffs.len - 1]
   mut i = coeffs.len - 2
   while i >= 0 {
      acc = _rat_add(_rat_mul_int(acc, x - Z(xs[i])), coeffs[i])
      i -= 1
   }
   _rat_to_mod(acc, modulus)
}

fn poly_pcg_advance_from_states(list states, int steps, any modulus=(Z(1) << 128), int min_window=16) bigint {
   "Predict `steps` future states from observed consecutive polynomial-congruential states."
   assert(states.len > 1, "at least two observed states")
   def window = poly_pcg_choose_window(states, modulus, min_window)
   def start = states.len - window - 1
   def xs = slice(states, start, states.len - 1)
   def ys = slice(states, start + 1, states.len)
   def coeffs = poly_pcg_newton_coeffs(xs, ys)
   mut cur = Z(states[states.len - 1])
   mut i = 0
   while i < steps {
      cur = poly_pcg_eval_next(cur, xs, coeffs, modulus)
      i += 1
   }
   cur
}

#main {
   def inc = pcg32_stream_increment(54)
   def st0 = Z(42)
   def pair = pcg32_next(st0, inc)
   assert(pcg32_rewind(pair.get(0), inc) == st0, "pcg rewind")
   assert(pcg32_advance(st0, inc, 1) == pair.get(0), "pcg advance")
   print("✓ std.math.crypto.prng.pcg self-test passed")
}
