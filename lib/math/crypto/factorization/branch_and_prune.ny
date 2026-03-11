;; Keywords: factorization branch-and-prune
;; Integer-factorization routines for branch-and-prune recovery of RSA factors and private exponents.
;; Reference:
;; - Heninger N., Shacham H., "Reconstructing RSA Private Keys from Random Key Bits"
module std.math.crypto.factorization.branch_and_prune(factorize_pq, factorize_pqd, factorize_pqddpdq)
use std.core
use std.math.nt
use std.math.crypto.number.partial

fn _bits_to_int_le(list: bits, int: upto): any {
   mut out = 0
   mut i = 0
   while(i < upto && i < bits.len){
      def b = bits.get(i)
      if(b != nil && b != "?"){ out += int(b) << i }
      i += 1
   }
   out
}

fn _partial_to_bits(any: pi): any {
   if(!is_list(pi) || pi.len < 3){ return nil }
   def components = pi.get(2)
   if(!is_list(components)){ return nil }
   mut bits = []
   mut ci = 0
   while(ci < components.len){
      def c = components.get(ci)
      if(!is_list(c) || c.len < 2){ return nil }
      def cv = c.get(0)
      def n = c.get(1)
      mut i = 0
      while(i < n){
         bits = bits.append(cv == nil ? nil : ((cv >> i) & 1))
         i += 1
      }
      ci += 1
   }
   bits
}

fn _tau(any: x): int {
   mut v, i = x, 0
   while(v != 0 && (v % 2) == 0){
      v /= 2
      i += 1
   }
   i
}

fn _known_bit_match(any: known, any: guess): int { (known != nil && known == guess) ? 1 : 0 }

fn _bit_lo(any: bit): any { bit == nil ? 0 : bit }

fn _bit_hi(any: bit): any { bit == nil ? 1 : bit }

fn _shifted_bit(list: bits, int: i): any { i >= bits.len ? 0 : bits.get(i) }

fn _restore_pq(list: p_bits, list: q_bits, int: i, any: p_prev, any: q_prev): any {
   p_bits[i] = p_prev
   q_bits[i] = q_prev
   nil
}

fn _set_if_present(list: bits, int: i, any: bit): any {
   if(i < bits.len){ bits[i] = bit }
   nil
}

fn _find_k(any: n, any: e, list: d_bits): list {
   mut best_score = -1
   mut best_k = nil
   mut best_bits = nil
   mut k = 1
   while(k < e){
      def d_guess = (k * (n + 1) + 1) / e
      mut guess_bits = list(d_bits.len)
      mut i = 0
      while(i < d_bits.len){
         guess_bits[i] = (d_guess >> i) & 1
         i += 1
      }
      mut score = 0
      i = (d_bits.len / 2) + 2
      while(i < d_bits.len){
         score += _known_bit_match(d_bits.get(i), guess_bits.get(i))
         i += 1
      }
      if(score > best_score){
         best_score = score
         best_k = k
         best_bits = guess_bits
      }
      k += 1
   }
   [best_k, best_bits]
}

fn _correct_msb(list: d_bits, any: guess_bits): any {
   mut i = (d_bits.len / 2) + 2
   while(i < d_bits.len){
      d_bits[i] = d_bits.get(i) == nil ? guess_bits.get(i) : d_bits.get(i)
      i += 1
   }
   nil
}

fn _correct_lsb(any: e, list: bits, int: exp): any {
   def inv = inverse_mod(e, 1 << exp)
   mut i = 0
   while(i < exp && i < bits.len){
      bits[i] = (inv >> i) & 1
      i += 1
   }
   nil
}

fn _ordered_factors(any: p, any: q): list { (p < q) ? [p, q] : [q, p] }

fn _valid_pq_bits(any: p_bits, any: q_bits): int { p_bits != nil && q_bits != nil && p_bits.len == q_bits.len }

fn _force_odd_pq(list: p_bits, list: q_bits): any {
   p_bits[0] = 1
   q_bits[0] = 1
   nil
}

fn _finish_factor_pair(any: n, any: r): any {
   if(r == nil){ return nil }
   def p, q = r.get(0), r.get(1)
   if(p * q != n){ return nil }
   _ordered_factors(p, q)
}

fn _prepare_d_bits(any: n, any: e, list: d_bits): any {
   def kg = _find_k(n, e, d_bits)
   def k, guess = kg.get(0), kg.get(1)
   if(k == nil || guess == nil){ return nil }
   _correct_msb(d_bits, guess)
   def tk = _tau(k)
   _correct_lsb(e, d_bits, 2 + tk)
   [k, tk]
}

fn _bp_pq(any: n, list: p_bits, list: q_bits, any: p_cur, any: q_cur, int: i): any {
   if(i >= p_bits.len || i >= q_bits.len){ return [p_cur, q_cur] }
   def c1 = ((n - p_cur * q_cur) >> i) & 1
   def p_prev, q_prev = p_bits.get(i), q_bits.get(i)
   def p0, p1 = _bit_lo(p_prev), _bit_hi(p_prev)
   def q0, q1 = _bit_lo(q_prev), _bit_hi(q_prev)
   mut p_bit = p0
   while(p_bit <= p1){
      mut q_bit = q0
      while(q_bit <= q1){
         if((p_bit ^^ q_bit) == c1){
            p_bits[i] = p_bit
            q_bits[i] = q_bit
            def r = _bp_pq(n, p_bits, q_bits, p_cur | (p_bit << i), q_cur | (q_bit << i), i + 1)
            if(r != nil){ return r }
         }
         q_bit += 1
      }
      p_bit += 1
   }
   _restore_pq(p_bits, q_bits, i, p_prev, q_prev)
   nil
}

fn _bp_pqd(any: n, any: e, any: k, int: tk, list: p_bits, list: q_bits, list: d_bits, any: p_cur, any: q_cur, int: i): any {
   if(i >= p_bits.len || i >= q_bits.len){ return [p_cur, q_cur] }
   def d_cur = _bits_to_int_le(d_bits, i)
   def c1 = ((n - p_cur * q_cur) >> i) & 1
   def c2 = ((k * (n + 1) + 1 - k * (p_cur + q_cur) - e * d_cur) >> (i + tk)) & 1
   def p_prev, q_prev = p_bits.get(i), q_bits.get(i)
   def di = i + tk
   def d_prev = _shifted_bit(d_bits, di)
   def p0, p1 = _bit_lo(p_prev), _bit_hi(p_prev)
   def q0, q1 = _bit_lo(q_prev), _bit_hi(q_prev)
   def d0, d1 = _bit_lo(d_prev), _bit_hi(d_prev)
   mut p_bit = p0
   while(p_bit <= p1){
      mut q_bit = q0
      while(q_bit <= q1){
         mut d_bit = d0
         while(d_bit <= d1){
            if((p_bit ^^ q_bit) == c1 && (d_bit ^^ p_bit ^^ q_bit) == c2){
               p_bits[i] = p_bit
               q_bits[i] = q_bit
               _set_if_present(d_bits, di, d_bit)
               def r = _bp_pqd(n, e, k, tk, p_bits, q_bits, d_bits, p_cur | (p_bit << i), q_cur | (q_bit << i), i + 1)
               if(r != nil){ return r }
            }
            d_bit += 1
         }
         q_bit += 1
      }
      p_bit += 1
   }
   _restore_pq(p_bits, q_bits, i, p_prev, q_prev)
   _set_if_present(d_bits, di, d_prev)
   nil
}

fn _bp_pqddpdq(any: n, any: e, any: k, int: tk, any: kp, int: tkp, any: kq, int: tkq, list: p_bits, list: q_bits, list: d_bits, list: dp_bits, list: dq_bits, any: p_cur, any: q_cur, int: i): any {
   if(i >= p_bits.len || i >= q_bits.len){ return [p_cur, q_cur] }
   def d_cur = _bits_to_int_le(d_bits, i)
   def dp_cur = _bits_to_int_le(dp_bits, i)
   def dq_cur = _bits_to_int_le(dq_bits, i)
   def c1 = ((n - p_cur * q_cur) >> i) & 1
   def c2 = ((k * (n + 1) + 1 - k * (p_cur + q_cur) - e * d_cur) >> (i + tk)) & 1
   def c3 = ((kp * (p_cur - 1) + 1 - e * dp_cur) >> (i + tkp)) & 1
   def c4 = ((kq * (q_cur - 1) + 1 - e * dq_cur) >> (i + tkq)) & 1
   def p_prev, q_prev = p_bits.get(i), q_bits.get(i)
   def di, dpi, dqi = i + tk, i + tkp, i + tkq
   def d_prev, dp_prev, dq_prev = _shifted_bit(d_bits, di), _shifted_bit(dp_bits, dpi), _shifted_bit(dq_bits, dqi)
   def p0, p1 = _bit_lo(p_prev), _bit_hi(p_prev)
   def q0, q1 = _bit_lo(q_prev), _bit_hi(q_prev)
   def d0, d1 = _bit_lo(d_prev), _bit_hi(d_prev)
   def dp0, dp1 = _bit_lo(dp_prev), _bit_hi(dp_prev)
   def dq0, dq1 = _bit_lo(dq_prev), _bit_hi(dq_prev)
   mut p_bit = p0
   while(p_bit <= p1){
      mut q_bit = q0
      while(q_bit <= q1){
         mut d_bit = d0
         while(d_bit <= d1){
            mut dp_bit = dp0
            while(dp_bit <= dp1){
               mut dq_bit = dq0
               while(dq_bit <= dq1){
                  if(
                     (p_bit ^^ q_bit) == c1 && (d_bit ^^ p_bit ^^ q_bit) == c2 &&
                     (dp_bit ^^ p_bit) == c3 && (dq_bit ^^ q_bit) == c4
                  ){
                     p_bits[i] = p_bit
                     q_bits[i] = q_bit
                     _set_if_present(d_bits, di, d_bit)
                     _set_if_present(dp_bits, dpi, dp_bit)
                     _set_if_present(dq_bits, dqi, dq_bit)
                     def r = _bp_pqddpdq(n, e, k, tk, kp, tkp, kq, tkq, p_bits, q_bits, d_bits, dp_bits, dq_bits, p_cur | (p_bit << i), q_cur | (q_bit << i), i + 1)
                     if(r != nil){ return r }
                  }
                  dq_bit += 1
               }
               dp_bit += 1
            }
            d_bit += 1
         }
         q_bit += 1
      }
      p_bit += 1
   }
   _restore_pq(p_bits, q_bits, i, p_prev, q_prev)
   _set_if_present(d_bits, di, d_prev)
   _set_if_present(dp_bits, dpi, dp_prev)
   _set_if_present(dq_bits, dqi, dq_prev)
   nil
}

fn factorize_pq(any: n, any: p_partial, any: q_partial): any {
   "Factor n when partial bits of p and q are known.
   Returns [p, q] with p <= q, or nil."
   def p_bits, q_bits = _partial_to_bits(p_partial), _partial_to_bits(q_partial)
   if(!_valid_pq_bits(p_bits, q_bits)){ return nil }
   _force_odd_pq(p_bits, q_bits)
   _finish_factor_pair(n, _bp_pq(n, p_bits, q_bits, 1, 1, 1))
}

fn factorize_pqd(any: n, any: e, any: p_partial, any: q_partial, any: d_partial): any {
   "Factor n when partial bits of p, q, and d are known.
   Returns [p, q] with p <= q, or nil."
   def p_bits, q_bits = _partial_to_bits(p_partial), _partial_to_bits(q_partial)
   def d_bits = _partial_to_bits(d_partial)
   if(!_valid_pq_bits(p_bits, q_bits) || d_bits == nil){ return nil }
   _force_odd_pq(p_bits, q_bits)
   def kd = _prepare_d_bits(n, e, d_bits)
   if(kd == nil){ return nil }
   def k, tk = kd.get(0), kd.get(1)
   _finish_factor_pair(n, _bp_pqd(n, e, k, tk, p_bits, q_bits, d_bits, 1, 1, 1))
}

fn factorize_pqddpdq(any: n, any: e, any: p_partial, any: q_partial, any: d_partial, any: dp_partial, any: dq_partial): any {
   "Factor n when partial bits of p, q, d, dp, and dq are known.
   Returns [p, q] with p <= q, or nil."
   def p_bits, q_bits = _partial_to_bits(p_partial), _partial_to_bits(q_partial)
   def d_bits = _partial_to_bits(d_partial)
   if(!_valid_pq_bits(p_bits, q_bits) || d_bits == nil){ return nil }
   _force_odd_pq(p_bits, q_bits)
   def kd = _prepare_d_bits(n, e, d_bits)
   if(kd == nil){ return nil }
   def k, tk = kd.get(0), kd.get(1)
   mut kp = 0
   while(kp < e){
      def poly = mod(kp * kp - kp * (k * (n - 1) + 1) - k, e)
      if(poly == 0){
         def inv = inverse_mod(kp, e)
         if(inv > 0){
            def kq = mod((0 - inv) * k, e)
            def dp_bits = _partial_to_bits(dp_partial)
            def dq_bits = _partial_to_bits(dq_partial)
            if(dp_bits != nil && dq_bits != nil){
               def tkp, tkq = _tau(kp), _tau(kq)
               _correct_lsb(e, dp_bits, 1 + tkp)
               _correct_lsb(e, dq_bits, 1 + tkq)
               def r = _finish_factor_pair(n, _bp_pqddpdq(n, e, k, tk, kp, tkp, kq, tkq, p_bits, q_bits, d_bits, dp_bits, dq_bits, 1, 1, 1))
               if(r != nil){ return r }
            }
         }
      }
      kp += 1
   }
   nil
}
