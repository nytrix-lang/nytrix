;; Keywords: prng lfsr math crypto
;; PRNG analysis routines for LFSR generation, Berlekamp-Massey, and state recovery.
;; Berlekamp-Massey algorithm to recover the minimal LFSR from a known bitstream.
;; Reference:
;; - https://en.wikipedia.org/wiki/Linear-feedback_shift_register
;; - https://en.wikipedia.org/wiki/Berlekamp%E2%80%93Massey_algorithm
;; References:
;; - std.math.crypto.prng
;; - std.math.crypto
module std.math.crypto.prng.lfsr(lfsr_next, lfsr_run, lfsr_keystream, lfsr_sequence, lfsr_autocorrelation, lfsr_connection_polynomial, lfsr_connection_polynomial_mod, lfsr_berlekamp_massey, lfsr_berlekamp_massey_mod, berlekamp_massey_mod, lfsr_polynomial_str, lfsr_connection_polynomial_str, lfsr_berlekamp_massey_polynomial_str, lfsr_rewind_sequence, lfsr_recover_state, lfsr_crack_from_output)
use std.core
use std.math.nt

fn _lfsr_modp(any x, any p) any {
   def r = x % p
   if r < 0 { return r + p }
   r
}

fn lfsr_next(any state, list taps, int n_bits) list {
   "Compute one LFSR clock step. Returns [output_bit, new_state].
   state: integer representing register contents(LSB = bit 0 output).
   taps: list of tap positions(1-indexed from LSB, i.e. feedback polynomial).
   n_bits: register width."
   def out = state & 1
   mut fb = 0
   mut i = 0
   while i < taps.len {
      def t = taps.get(i)
      fb = fb ^^ ((state >> (t - 1)) & 1)
      i += 1
   }
   def new_state = (fb << (n_bits - 1)) | (state >> 1)
   [out, new_state]
}

fn lfsr_run(any initial_state, list taps, int n_bits, int steps) list {
   "Run LFSR for given number of steps from initial_state.
   Returns [bit_list, final_state]."
   mut state = initial_state
   mut bits = []
   mut i = 0
   while i < steps {
      def r = lfsr_next(state, taps, n_bits)
      bits = bits.append(r.get(0))
      state = r.get(1)
      i += 1
   }
   [bits, state]
}

fn lfsr_keystream(any initial_state, list taps, int n_bits, int length) list {
   "Generate a keystream of `length` bits from the LFSR.
   Returns the bit list."
   def r = lfsr_run(initial_state, taps, n_bits, length)
   r.get(0)
}

fn lfsr_sequence(list key, list fill, int n, any p=2) list {
   "Reference LFSR sequence over Z/pZ.
   key and fill are coefficient/state lists over Z/pZ. The output emits the
   leftmost state entry each step and appends sum(key[i] * old_state[i])."
   if !is_list(key) || !is_list(fill) { panic("lfsr_sequence: key and fill must be lists") }
   def k = fill.len
   if key.len != k { panic("lfsr_sequence: key and fill must have the same length") }
   mut state = clone(fill)
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(mod(state.get(0), p))
      mut fb = 0
      mut j = 0
      while j < k {
         fb = mod(fb + key.get(j) * state.get(j), p)
         j += 1
      }
      mut next = []
      j = 1
      while j < k {
         next = next.append(state.get(j))
         j += 1
      }
      next = next.append(fb)
      state = next
      i += 1
   }
   out
}

fn lfsr_autocorrelation(list xs, any period, any shift) list {
   "Return autocorrelation numerator/denominator [sum(seq[i]*seq[i+k]), period]."
   if !is_list(xs) { panic("lfsr_autocorrelation: sequence must be a list") }
   def p, k = int(period), int(shift)
   if p <= 0 || xs.len < p { panic("lfsr_autocorrelation: invalid period") }
   mut num = 0
   mut i = 0
   while i < p {
      num += int(xs.get(i)) * int(xs.get((i + k) % p))
      i += 1
   }
   [num, p]
}

fn lfsr_berlekamp_massey_mod(list xs, any p) list {
   "Berlekamp-Massey over GF(p). Returns `[linear_complexity, connection_coeffs]`.
   `connection_coeffs` are constant-first and satisfy
   `s[n] + c[1]*s[n-1] + ... + c[L]*s[n-L] = 0 mod p`."
   if p <= 1 { panic("lfsr_berlekamp_massey_mod: modulus must be > 1") }
   def n = xs.len
   mut C, B = [1], [1]
   mut L, m = 0, 1
   mut b = 1
   mut pos = 0
   while pos < n {
      mut d, i = _lfsr_modp(xs.get(pos), p), 1
      while i <= L {
         if i < C.len { d = _lfsr_modp(d + C.get(i) * xs.get(pos - i), p) }
         i += 1
      }
      if d == 0 {
         m += 1
      } else {
         def T = clone(C)
         def inv_b = inverse_mod(_lfsr_modp(b, p), p)
         if inv_b == nil { panic("lfsr_berlekamp_massey_mod: non-invertible discrepancy") }
         def coef = _lfsr_modp(d * inv_b, p)
         mut bi = 0
         while bi < B.len {
            def cpos = bi + m
            while C.len <= cpos { C = C.append(0) }
            C.set(cpos, _lfsr_modp(C.get(cpos) - coef * B.get(bi), p))
            bi += 1
         }
         if 2 * L <= pos {
            L, B = pos + 1 - L, T
            b, m = d, 1
         } else {
            m += 1
         }
      }
      pos += 1
   }
   [L, C]
}

fn lfsr_berlekamp_massey(list bits) list {
   "Find the minimal LFSR that generates the given bit sequence using the
   Berlekamp-Massey algorithm. Works over GF(2).
   bits: list of 0/1 values.
   Returns [lfsr_length, connection_polynomial_coefficients] where
   coefficients are a list of 0/1 values(index 0 = constant term = 1)."
   lfsr_berlekamp_massey_mod(bits, 2)
}

fn lfsr_connection_polynomial_mod(list xs, any p) list {
   "Return minimal polynomial coefficients over GF(p), constant-first and monic."
   def bm = lfsr_berlekamp_massey_mod(xs, p)
   def c = bm.get(1)
   mut out = []
   mut i = c.len - 1
   while i >= 0 {
      out = out.append(_lfsr_modp(c.get(i), p))
      i -= 1
   }
   out
}

fn berlekamp_massey_mod(list xs, any p) list {
   "Return the monic minimal polynomial over GF(p), constant-first."
   lfsr_connection_polynomial_mod(xs, p)
}

fn lfsr_connection_polynomial(list bits) list {
   "Return the connection polynomial coefficients for a GF(2) sequence.
   This is the reverse of the Berlekamp-Massey polynomial representation used
   by `lfsr_berlekamp_massey`."
   lfsr_connection_polynomial_mod(bits, 2)
}

fn lfsr_polynomial_str(list coeffs, str variable="x") str {
   "Format monic polynomial coefficients in descending-degree order."
   mut out = ""
   def degree = coeffs.len - 1
   mut i = 0
   while i < coeffs.len {
      def c = int(coeffs.get(i))
      if c != 0 {
         def exp = degree - i
         mut term = ""
         if exp == 0 {
            term = to_str(c)
         } elif exp == 1 {
            term = c == 1 ? variable : (to_str(c) + "*" + variable)
         } else {
            term = c == 1 ? (variable + "^" + to_str(exp)) : (to_str(c) + "*" + variable + "^" + to_str(exp))
         }
         out = out.len == 0 ? term : (out + " + " + term)
      }
      i += 1
   }
   out.len == 0 ? "0" : out
}

fn lfsr_connection_polynomial_str(list bits, str variable="x") str {
   "Return the Sage-style connection polynomial string for a GF(2) sequence."
   lfsr_polynomial_str(lfsr_connection_polynomial(bits), variable)
}

fn lfsr_berlekamp_massey_polynomial_str(list bits, str variable="x") str {
   "Return the Sage-style Berlekamp-Massey polynomial string for a GF(2) sequence."
   def bm = lfsr_berlekamp_massey(bits)
   lfsr_polynomial_str(bm.get(1), variable)
}

fn lfsr_rewind_sequence(list window, list connection_coeffs, int steps) list {
   "Rewind a GF(2) LFSR output window by `steps` clocks.
   `window` is `[s[t], ..., s[t+L-1]]`.
   `connection_coeffs` is the Berlekamp-Massey form `[1, c1, ..., cL]`
   satisfying `s[n] + c1*s[n-1] + ... + cL*s[n-L] = 0`."
   def L = connection_coeffs.len - 1
   if L <= 0 || window.len < L { panic("lfsr_rewind_sequence: invalid window") }
   if (int(connection_coeffs[L]) & 1) == 0 { panic("lfsr_rewind_sequence: recurrence is not reversible") }
   mut state = slice(window, 0, L)
   mut step = 0
   while step < steps {
      mut prev = int(state[L - 1]) & 1
      mut i = 1
      while i < L {
         if (int(connection_coeffs[i]) & 1) == 1 {
            prev = prev ^^ (int(state[L - 1 - i]) & 1)
         }
         i += 1
      }
      mut next = [prev]
      i = 0
      while i < L - 1 {
         next = next.append(int(state[i]) & 1)
         i += 1
      }
      state = next
      step += 1
   }
   state
}

fn lfsr_recover_state(list bits, list taps, int n_bits) any {
   "Recover the LFSR initial state from known output bits.
   Tries all 2^n_bits possible initial states and returns the one
   that produces the observed bits(or nil if not found).
   Practical only for small n_bits(up to ~20)."
   if n_bits > 24 { return nil }
   def max_state = 1 << n_bits
   mut s = 1
   while s < max_state {
      def result = lfsr_run(s, taps, n_bits, bits.len)
      def out_bits = result.get(0)
      mut ok = true
      mut i = 0
      while i < bits.len {
         if out_bits.get(i) != bits.get(i) {
            ok = false
            i = bits.len
         }
         i += 1
      }
      if ok { return s }
      s += 1
   }
   nil
}

fn lfsr_crack_from_output(list bits) list {
   "Crack LFSR parameters from known output bitstream using Berlekamp-Massey.
   Returns [length, polynomial] where polynomial coefficients indicate taps.
   Use the returned length and polynomial to predict future bits."
   lfsr_berlekamp_massey(bits)
}
