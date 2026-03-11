;; Keywords: factorization pollard
;; Integer-factorization routines for Pollard rho, p-1, and related factorization.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.factorization.pollard(pollard_rho, pollard_brent, pollard_brent_report, pollard_rho_iter, pollard_pm1, pollard_pm1_report, pollard_pm1_stage2, pollard_pm1_stage2_report, pollard_p1, pollard_p1_report, williams_pp1, williams_pp1_report, squfof, squfof_report, lehman_factor, lehman_factor_report, pollard_strassen, pollard_strassen_report, batch_gcd_factor_report, batch_gcd_factor, batch_gcd_tree_factor_report, batch_gcd_tree_factor, full_factorization_rho)
use std.math.nt
use std.math.simmd as simmd
use std.math.crypto.number.lucas
use std.math.crypto.factorization.fermat as fermat
use std.os.clock (ticks)

mut _pollard_small_primes_cache = nil
mut _pollard_prime_powers_cache = nil
mut _pollard_small_primes_last_bound = 0
mut _pollard_small_primes_last_value = nil
mut _pollard_prime_powers_last_bound = 0
mut _pollard_prime_powers_last_value = nil

fn _pollard_rho_floyd_d(any: n, int: max_iter): any {
   def g = lambda(any: x): any { (x * x + 1) % n }
   mut x, y = 2, 2
   mut d = 1
   mut iters = 0
   while(d == 1 && iters < max_iter){
      x, y = g(x), g(g(y))
      def diff = (x > y) ? (x - y) : (y - x)
      d = gcd(diff, n)
      iters += 1
   }
   d
}

fn _pollard_elapsed_ms(any: t0): f64 { float(ticks() - t0) / 1000000.0 }

fn _pollard_finish(dict: out, any: t0, str: status): dict {
   out.merge({"elapsed_ms": _pollard_elapsed_ms(t0), "status": status})
}

fn _pollard_finish_factor(dict: out, any: n, any: f, any: t0, str: status): dict {
   _pollard_finish(out.merge({"factor": f, "cofactor": Z(n) / Z(f), "success": true}), t0, status)
}

fn _pollard_report(str: method, any: nz): dict {
   {"method": method, "n_bits": bit_length(nz), "success": false, "factor": nil}
}

fn _pollard_small_primes_uncached(int: bound): list {
   if(bound < 2){ return [] }
   mut sieve = list(bound + 1)
   __list_set_len(sieve, bound + 1)
   mut i = 0
   while(i <= bound){
      __store_item_fast(sieve, i, true)
      i += 1
   }
   __store_item_fast(sieve, 0, false)
   __store_item_fast(sieve, 1, false)
   mut p = 2
   while(p * p <= bound){
      if(sieve[p]){
         mut m = p * p
         while(m <= bound){
            __store_item_fast(sieve, m, false)
            m += p
         }
      }
      p += (p == 2) ? 1 : 2
   }
   mut primes = list(max(1, bound / 8))
   if(bound >= 2){ primes = primes.append(2) }
   i = 3
   while(i <= bound){
      if(sieve[i]){ primes = primes.append(i) }
      i += 2
   }
   primes
}

fn _pollard_small_primes(int: bound): list {
   if(bound < 2){ return [] }
   if(_pollard_small_primes_last_value != nil && bound == _pollard_small_primes_last_bound){ return _pollard_small_primes_last_value }
   if(_pollard_small_primes_cache == nil){ _pollard_small_primes_cache = dict(8) }
   def key = to_str(bound)
   if(_pollard_small_primes_cache.contains(key)){
      _pollard_small_primes_last_bound = bound
      _pollard_small_primes_last_value = _pollard_small_primes_cache.get(key, [])
      return _pollard_small_primes_last_value
   }
   def primes = _pollard_small_primes_uncached(bound)
   _pollard_small_primes_cache[key] = primes
   _pollard_small_primes_last_bound = bound
   _pollard_small_primes_last_value = primes
   primes
}

fn _pollard_prime_powers(int: bound): list {
   if(bound < 2){ return [] }
   if(_pollard_prime_powers_last_value != nil && bound == _pollard_prime_powers_last_bound){ return _pollard_prime_powers_last_value }
   if(_pollard_prime_powers_cache == nil){ _pollard_prime_powers_cache = dict(8) }
   def key = to_str(bound)
   if(_pollard_prime_powers_cache.contains(key)){
      _pollard_prime_powers_last_bound = bound
      _pollard_prime_powers_last_value = _pollard_prime_powers_cache.get(key, [])
      return _pollard_prime_powers_last_value
   }
   def bz = Z(bound)
   def primes = _pollard_small_primes(bound)
   mut powers = list(primes.len)
   mut i = 0
   while(i < primes.len){
      def p = Z(primes[i])
      mut q = p
      while(q * p <= bz){ q = q * p }
      powers = powers.append(q)
      i += 1
   }
   _pollard_prime_powers_cache[key] = powers
   _pollard_prime_powers_last_bound = bound
   _pollard_prime_powers_last_value = powers
   powers
}

fn _pollard_pm1_stage1_state(any: n, int: bound): dict {
   "Return the p-1 stage-1 accumulator using prime-power exponents up to bound."
   def nz = Z(n)
   mut b = bound
   if(b < 2){ b = 2 }
   mut a = Z(2)
   def powers = _pollard_prime_powers(b)
   mut i = 0
   mut ops = 0
   mut last_power = Z(1)
   while(i < powers.len){
      def q = powers[i]
      a = power_mod(a, q, nz)
      last_power = q
      ops += 1
      i += 1
   }
   {
      "a": a, "stage1_ops": ops, "stage1_prime_count": powers.len,
      "stage1_last_prime_power": last_power,
   }
}

fn _pollard_record_pm1_stage1(dict: out, dict: st, any: d): dict {
   out.merge({
         "stage1_ops": st.get("stage1_ops", 0),
         "stage1_prime_count": st.get("stage1_prime_count", 0),
         "stage1_last_prime_power": st.get("stage1_last_prime_power", Z(1)),
         "stage1_gcd": d,
   })
}

fn pollard_rho(any: n): any {
   "Pollard rho algorithm for finding a non-trivial factor of n.
   Uses Floyd cycle detection with f(x) = x^2 + 1 mod n.
   Falls back to Brent if Floyd does not yield a non-trivial factor."
   def nz = _z(n)
   if(nz <= 1){ return nil }
   if(nz % 2 == 0){ return Z(2) }
   def d = _pollard_rho_floyd_d(nz, 200000)
   if(_is_nontrivial_factor(d, nz)){ return d }
   pollard_brent(nz)
}

fn _abs_z(any: x): any {
   def z = Z(x)
   z < 0 ? -z : z
}

fn _z(any: x): any { is_bigint(x) ? x : Z(x) }

@inline
fn _gcd_int(int: a0, int: b0): int {
   mut a = a0 < 0 ? -a0 : a0
   mut b = b0 < 0 ? -b0 : b0
   while(b > 0){
      def r = a % b
      a, b = b, r
   }
   a
}

fn _is_nontrivial_factor(any: g, any: n): bool {
   def gz, nz = _z(g), _z(n)
   gz > 1 && gz < nz && nz % gz == 0
}

fn _pollard_brent_fermat_precheck_budget(any: n): int {
   case bit_length(Z(n)){
      67..69 -> 4096
      70..72 -> 256
      73..75 -> 1024
      76..79 -> 2560
      80..81 -> 1024
      82..96 -> 4096
      _ -> 0
   }
}

fn _pollard_brent_pm1_precheck_bound(any: n): int {
   case bit_length(Z(n)){
      80..81 -> 1024
      _ -> 0
   }
}

fn _pollard_brent_tuned_precheck_params(any: n, any: y0, any: c0, any: m0, int: max_gcds): list {
   if(max_gcds < 200000 || int(m0) < 128 || Z(y0) != Z(2) || Z(c0) != Z(1)){ return [Z(0), Z(0), 0, 0] }
   case bit_length(Z(n)){
      77 -> [Z(11), Z(17), 16, 8]
      _ -> [Z(0), Z(0), 0, 0]
   }
}

@inline
fn _pollard_iabs(int: x): int { x < 0 ? -x : x }

fn _pollard_brent_factor_i31(int: n, int: y0=2, int: c0=1, int: m0=128, int: max_gcds=200000): list<int> {
   if(n <= 1){ return [0, 0, 0, 0] }
   if((n & 1) == 0){ return [2, 0, 0, 1] }
   mut y = y0 % n
   mut c = c0 % n
   if(y < 0){ y += n }
   if(c < 0){ c += n }
   mut m = m0
   if(m < 1){ m = 1 }
   mut g = 1
   mut r = 1
   mut q = 1
   mut x = 0
   mut ys = 0
   mut gcds = 0
   while(g == 1 && gcds < max_gcds){
      x = y
      mut i = 0
      while(i < r){
         y = (y * y + c) % n
         i += 1
      }
      mut k = 0
      while(k < r && g == 1 && gcds < max_gcds){
         ys = y
         def lim = (m < (r - k)) ? m : (r - k)
         i = 0
         while(i < lim){
            y = (y * y + c) % n
            q = (q * _pollard_iabs(x - y)) % n
            i += 1
         }
         g = _gcd_int(q, n)
         gcds += 1
         k += m
      }
      r = r * 2
   }
   if(g == 1){ return [0, gcds, 0, 2] }
   mut fallback = 0
   if(g == n){
      while(fallback < max_gcds){
         ys = (ys * ys + c) % n
         g = _gcd_int(_pollard_iabs(x - ys), n)
         if(g > 1){ break }
         fallback += 1
      }
   }
   if(g > 1 && g < n && n % g == 0){ return [g, gcds, fallback, 1] }
   [0, gcds, fallback, 3]
}

fn _pollard_brent_report_i31(int: n, int: y0=2, int: c0=1, int: m0=128, int: max_gcds=200000): dict {
   def t0 = ticks()
   def nz = Z(n)
   mut out = _pollard_report("pollard-brent", nz).merge({
         "y0": y0, "c": c0, "block": m0, "max_gcds": max_gcds,
         "pm1_precheck_used": false, "pm1_precheck_B": 0, "pm1_precheck_status": "", "pm1_precheck_stage1_ops": 0,
         "fermat_precheck_used": false, "fermat_precheck_max_iters": 0, "fermat_precheck_status": "",
         "kernel": "i31-brent",
   })
   if(n <= 1){ return _pollard_finish(out, t0, "invalid-input") }
   if((n & 1) == 0){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   def r = _pollard_brent_factor_i31(n, y0, c0, m0, max_gcds)
   def f, gcds, fallback, status = int(r[0]), int(r[1]), int(r[2]), int(r[3])
   out = out.merge({"gcds": gcds, "fallback_gcds": fallback})
   if(status == 1){ return _pollard_finish_factor(out, nz, Z(f), t0, "factor") }
   if(status == 2){ return _pollard_finish(out, t0, "iteration-limit") }
   _pollard_finish(out, t0, "cycle-failed")
}

fn _pollard_brent_core(any: n, any: y0=2, any: c0=1, any: m0=128, int: max_gcds=200000): dict {
   def t0 = ticks()
   def nz = Z(n)
   mut out = _pollard_report("pollard-brent", nz).merge({
         "y0": y0, "c": c0, "block": m0, "max_gcds": max_gcds,
   })
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % 2 == 0){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   def tuned = _pollard_brent_tuned_precheck_params(nz, y0, c0, m0, max_gcds)
   def tuned_budget = int(tuned.get(3))
   if(tuned_budget > 0){
      def tuned_pre = _pollard_brent_core(nz, tuned.get(0), tuned.get(1), tuned.get(2), tuned_budget)
      out = out.merge({
            "tuned_precheck_used": true,
            "tuned_precheck_y0": tuned.get(0),
            "tuned_precheck_c": tuned.get(1),
            "tuned_precheck_block": tuned.get(2),
            "tuned_precheck_gcds": tuned_pre.get("gcds", 0),
            "tuned_precheck_status": tuned_pre.get("status", ""),
      })
      if(tuned_pre.get("success", false)){
         return _pollard_finish_factor(out, nz, tuned_pre.get("factor", nil), t0, "factor")
      }
   }
   def pm1_bound = (max_gcds >= 200000 && int(m0) >= 128) ? _pollard_brent_pm1_precheck_bound(nz) : 0
   if(pm1_bound > 0){
      out = out.merge({"pm1_precheck_used": true, "pm1_precheck_B": pm1_bound})
      def pm1 = pollard_pm1_report(nz, pm1_bound)
      def fp = pm1.get("factor", nil)
      if(_is_nontrivial_factor(fp, nz)){
         out = out.merge({
               "pm1_precheck_status": pm1.get("status", "factor"),
               "pm1_precheck_stage1_ops": pm1.get("stage1_ops", 0),
               "gcds": 0, "fallback_gcds": 0,
         })
         return _pollard_finish_factor(out, nz, fp, t0, "factor")
      }
      out = out.merge({
            "pm1_precheck_status": pm1.get("status", "smoothness-bound-exhausted"),
            "pm1_precheck_stage1_ops": pm1.get("stage1_ops", 0),
      })
   } else {
      out = out.merge({"pm1_precheck_used": false, "pm1_precheck_B": 0, "pm1_precheck_status": "", "pm1_precheck_stage1_ops": 0})
   }
   def fermat_budget = (max_gcds >= 200000 && int(m0) >= 128) ? _pollard_brent_fermat_precheck_budget(nz) : 0
   if(fermat_budget > 0){
      out = out.merge({"fermat_precheck_used": true, "fermat_precheck_max_iters": fermat_budget})
      def close = fermat.fermat_factor_bounded(nz, fermat_budget)
      if(close != nil){
         def f = close.get(0)
         if(_is_nontrivial_factor(f, nz)){
            out = out.merge({"fermat_precheck_status": "factor", "gcds": 0, "fallback_gcds": 0})
            return _pollard_finish_factor(out, nz, f, t0, "factor")
         }
      }
      out = out.set("fermat_precheck_status", "iteration-limit")
   } else {
      out = out.merge({"fermat_precheck_used": false, "fermat_precheck_max_iters": 0, "fermat_precheck_status": ""})
   }
   mut y, c = Z(y0), Z(c0)
   mut m = int(m0)
   if(m < 1){ m = 1 }
   mut g, r = Z(1), 1
   mut q, x = Z(1), Z(0)
   mut ys = Z(0)
   mut gcds = 0
   while(g == 1 && gcds < max_gcds){
      x = y
      mut i = 0
      while(i < r){
         y = (y * y + c) % nz
         i += 1
      }
      mut k = 0
      while(k < r && g == 1 && gcds < max_gcds){
         ys = y
         def lim = (m < (r - k)) ? m : (r - k)
         i = 0
         while(i < lim){
            y, q = (y * y + c) % nz, (q * _abs_z(x - y)) % nz
            i += 1
         }
         g = gcd(q, nz)
         gcds += 1
         k += m
      }
      r = r * 2
   }
   out = out.set("gcds", gcds)
   if(g == Z(1)){ return _pollard_finish(out, t0, "iteration-limit") }
   if(g == nz){
      mut guard = 0
      while(guard < max_gcds){
         ys = (ys * ys + c) % nz
         g = gcd(_abs_z(x - ys), nz)
         if(g > 1){ break }
         guard += 1
      }
      out = out.set("fallback_gcds", guard)
   }
   if(_is_nontrivial_factor(g, nz)){ return _pollard_finish_factor(out, nz, g, t0, "factor") }
   _pollard_finish(out, t0, "cycle-failed")
}

fn pollard_brent_report(any: n, any: y0=2, any: c0=1, any: m0=128, int: max_gcds=200000): dict {
   "Return a bounded, debuggable Brent-rho attempt report."
   if(is_int(n) && int(n) <= 2147483647){ return _pollard_brent_report_i31(int(n), int(y0), int(c0), int(m0), max_gcds) }
   def nz = Z(n)
   if(bit_length(nz) <= 31){ return _pollard_brent_report_i31(bigint_to_int(nz), int(y0), int(c0), int(m0), max_gcds) }
   _pollard_brent_core(n, y0, c0, m0, max_gcds)
}

fn pollard_brent(any: n, any: y0=2, any: c0=1, any: m0=128, int: max_gcds=200000): any {
   "Brent's cycle variant of Pollard rho. Usually faster than Floyd rho."
   if(is_int(n) && int(n) <= 2147483647){
      def r = _pollard_brent_factor_i31(int(n), int(y0), int(c0), int(m0), max_gcds)
      return int(r[3]) == 1 ? Z(int(r[0])) : nil
   }
   def nz = Z(n)
   if(bit_length(nz) <= 31){
      def r = _pollard_brent_factor_i31(bigint_to_int(nz), int(y0), int(c0), int(m0), max_gcds)
      return int(r[3]) == 1 ? Z(int(r[0])) : nil
   }
   _pollard_brent_core(n, y0, c0, m0, max_gcds).get("factor", nil)
}

fn pollard_rho_iter(any: n, any: max_iter): any {
   "Pollard rho with an iteration limit to prevent infinite loops.
   Returns a factor of n, or nil if not found within max_iter iterations."
   def d = _pollard_rho_floyd_d(n, int(max_iter))
   _is_nontrivial_factor(d, n) ? d : nil
}

fn pollard_pm1(any: n, any: B): any {
   "Pollard p-1 algorithm: finds a factor p of n when p-1 is B-smooth.
   Computes a = 2^(lcm(1..B)) mod n, then gcd(a-1, n).
   Returns a non-trivial factor of n, or nil if none found."
   pollard_pm1_report(n, B).get("factor", nil)
}

fn pollard_pm1_report(any: n, any: B): dict {
   "Report a stage-1 Pollard p-1 attempt with prime-power exponent accounting."
   def t0 = ticks()
   def nz = Z(n)
   mut b = int(B)
   mut out = _pollard_report("pollard-p-minus-1", nz).merge({"B": b})
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   if(b < 2){ b = 2 }
   def st = _pollard_pm1_stage1_state(nz, b)
   def a = st.get("a", Z(2))
   def d = gcd(_abs_z(a - Z(1)), nz)
   out = out.set("B", b)
   out = _pollard_record_pm1_stage1(out, st, d)
   if(_is_nontrivial_factor(d, nz)){ return _pollard_finish_factor(out, nz, d, t0, "stage1-factor") }
   if(d == nz){ return _pollard_finish(out, t0, "stage1-degenerate") }
   _pollard_finish(out, t0, "smoothness-bound-exhausted")
}

fn pollard_pm1_stage2(any: n, any: B1, any: B2): any {
   "Pollard p-1 with stage 2 extension.
   Stage 1 uses bound B1, stage 2 checks primes in [B1+1, B2].
   Returns a non-trivial factor of n, or nil."
   pollard_pm1_stage2_report(n, B1, B2).get("factor", nil)
}

fn pollard_pm1_stage2_report(any: n, any: B1, any: B2): dict {
   "Report a Pollard p-1 attempt with batched prime stage 2."
   def t0 = ticks()
   def nz = Z(n)
   mut b1, b2 = int(B1), int(B2)
   mut out = _pollard_report("pollard-p-minus-1-stage2", nz).merge({"B1": b1, "B2": b2})
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   if(b1 < 2){ b1 = 2 }
   if(b2 < b1){ b2 = b1 }
   def st = _pollard_pm1_stage1_state(nz, b1)
   def a = st.get("a", Z(2))
   def d1 = gcd(_abs_z(a - Z(1)), nz)
   out = out.merge({"B1": b1, "B2": b2})
   out = _pollard_record_pm1_stage1(out, st, d1)
   if(_is_nontrivial_factor(d1, nz)){ return _pollard_finish_factor(out, nz, d1, t0, "stage1-factor") }
   if(d1 == nz){ return _pollard_finish(out, t0, "stage1-degenerate") }
   if(b2 <= b1){ return _pollard_finish(out, t0, "stage1-exhausted") }
   mut prod = Z(1)
   def primes2 = _pollard_small_primes(b2)
   mut pi = 0
   mut stage2_ops = 0
   mut batch_gcds = 0
   while(pi < primes2.len){
      def qi = primes2.get(pi)
      if(qi <= b1){
         pi += 1
         continue
      }
      def q = Z(qi)
      def term = power_mod(a, q, nz)
      prod = (prod * _abs_z(term - Z(1))) % nz
      stage2_ops += 1
      if(stage2_ops % 64 == 0){
         def gb = gcd(prod, nz)
         batch_gcds += 1
         if(_is_nontrivial_factor(gb, nz)){
            out = out.merge({"stage2_ops": stage2_ops, "stage2_batch_gcds": batch_gcds, "stage2_gcd": gb})
            return _pollard_finish_factor(out, nz, gb, t0, "stage2-batch-factor")
         }
         if(gb == nz){ prod = Z(1) }
      }
      pi += 1
   }
   def d2 = gcd(prod, nz)
   out = out.merge({
         "stage2_ops": stage2_ops, "stage2_prime_count": stage2_ops,
         "stage2_batch_gcds": batch_gcds + 1, "stage2_gcd": d2,
   })
   if(_is_nontrivial_factor(d2, nz)){ return _pollard_finish_factor(out, nz, d2, t0, "stage2-factor") }
   if(d2 == nz){
      pi = 0
      mut fallback_ops = 0
      while(pi < primes2.len){
         def qi = primes2.get(pi)
         if(qi <= b1){
            pi += 1
            continue
         }
         def q = Z(qi)
         def gi = gcd(_abs_z(power_mod(a, q, nz) - Z(1)), nz)
         fallback_ops += 1
         if(_is_nontrivial_factor(gi, nz)){
            out = out.merge({"stage2_fallback_ops": fallback_ops, "stage2_gcd": gi})
            return _pollard_finish_factor(out, nz, gi, t0, "stage2-individual-factor")
         }
         pi += 1
      }
      out = out.set("stage2_fallback_ops", fallback_ops)
      return _pollard_finish(out, t0, "stage2-degenerate")
   }
   _pollard_finish(out, t0, "smoothness-bound-exhausted")
}

fn pollard_p1(any: n, any: B): any {
   "Pollard p+1 algorithm: finds a factor p of n when p+1 is B-smooth.
   Uses Lucas sequences over the group of norm-1 elements.
   Returns a non-trivial factor of n, or nil if none found."
   pollard_p1_report(n, B).get("factor", nil)
}

fn _pollard_p1_with_D_report(any: n, int: B, any: D): dict {
   def t0 = ticks()
   def nz = Z(n)
   mut out = _pollard_report("pollard-p-plus-1-D", nz).merge({"B": B, "D": D})
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   def two = Z(2)
   def P_init = (Z(D) * two) % nz
   mut P = P_init
   def powers = _pollard_prime_powers(B)
   mut i = 0
   mut gcds = 0
   mut last_power = Z(1)
   while(i < powers.len){
      def q = powers[i]
      P = lucas_v_mod(P, q, nz, 1)
      last_power = q
      def diff = _abs_z(P - two)
      def d = gcd(diff, nz)
      gcds += 1
      if(_is_nontrivial_factor(d, nz)){
         out = out.merge({"iterations": i + 1, "gcds": gcds, "prime_power_count": powers.len, "last_prime_power": last_power})
         return _pollard_finish_factor(out, nz, d, t0, "factor")
      }
      i += 1
   }
   out = out.merge({"iterations": powers.len, "gcds": gcds, "prime_power_count": powers.len, "last_prime_power": last_power})
   _pollard_finish(out, t0, "smoothness-bound-exhausted")
}

fn pollard_p1_report(any: n, any: B): dict {
   "Report Pollard p+1 attempts across the standard Lucas D candidates."
   def t0 = ticks()
   def nz = Z(n)
   mut b = int(B)
   mut out = _pollard_report("pollard-p-plus-1", nz).merge({"B": b})
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   if(b < 2){ b = 2 }
   def D_list = [5, 7, 11, 13, 17, 19, 23, 29, 31]
   mut attempts = []
   mut i = 0
   while(i < D_list.len){
      def D = D_list[i]
      def jac = jacobi(D, nz)
      if(jac == -1){
         def r = _pollard_p1_with_D_report(nz, b, D)
         attempts = attempts.append(r)
         def f = r.get("factor", nil)
         if(_is_nontrivial_factor(f, nz)){
            out = out.merge({"attempts": attempts, "attempt_count": attempts.len, "D": D})
            return _pollard_finish_factor(out, nz, f, t0, "factor")
         }
      } else {
         attempts = attempts.append({"D": D, "jacobi": jac, "status": "jacobi-not-minus-one"})
      }
      i += 1
   }
   out = out.merge({"attempts": attempts, "attempt_count": attempts.len})
   _pollard_finish(out, t0, "smoothness-bound-exhausted")
}

fn pollard_p1_with_D(any: n, any: B, any: D): any {
   "Internal helper for Pollard p+1 using a specific D with jacobi(D, n) == -1.
   Uses Lucas sequence V_k(P, Q) with Q = 1 and P derived from D."
   def nz = Z(n)
   def two = Z(2)
   mut P = (Z(D) * two) % nz
   def powers = _pollard_prime_powers(int(B))
   mut i = 0
   while(i < powers.len){
      P = lucas_v_mod(P, powers[i], nz, 1)
      def d = gcd(_abs_z(P - two), nz)
      if(_is_nontrivial_factor(d, n)){ return d }
      i += 1
   }
   nil
}

fn williams_pp1(any: n, any: B=10000): any {
   "Williams p+1 factorization wrapper.
   Reuses the Lucas-sequence p+1 core with increasing bounds."
   williams_pp1_report(n, B).get("factor", nil)
}

fn williams_pp1_report(any: n, any: B=10000): dict {
   "Report Williams p+1 attempts with increasing bounds."
   def t0 = ticks()
   def nz = Z(n)
   mut bound = int(B)
   mut out = _pollard_report("williams-p-plus-1", nz).merge({"initial_bound": bound})
   if(nz <= Z(3)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   if(bound < 32){ bound = 32 }
   mut attempts = []
   mut tries = 0
   while(tries < 4){
      def r = pollard_p1_report(nz, bound)
      attempts = attempts.append(r)
      def f = r.get("factor", nil)
      if(_is_nontrivial_factor(f, nz)){
         out = out.merge({"attempts": attempts, "attempt_count": attempts.len, "final_bound": bound})
         return _pollard_finish_factor(out, nz, f, t0, "factor")
      }
      bound = bound * 2
      tries += 1
   }
   out = out.merge({"attempts": attempts, "attempt_count": attempts.len, "final_bound": bound / 2})
   _pollard_finish(out, t0, "smoothness-bound-exhausted")
}

fn _ceil_isqrt_z(any: n): bigint {
   def s = isqrt(n)
   s * s == n ? s : s + Z(1)
}

fn _lehman_square_residue_ok(any: x): bool {
   def z = Z(x)
   def r64 = int(z & Z(63))
   if(!(r64 == 0 || r64 == 1 || r64 == 4 || r64 == 9 || r64 == 16 || r64 == 17 || r64 == 25 || r64 == 33 || r64 == 36 || r64 == 41 || r64 == 49 || r64 == 57)){ return false }
   def r105 = int(z % Z(105))
   def ok105 = case r105 {
      0, 1, 4, 9, 15, 16, 21, 25, 30, 36, 39, 46, 49, 51,
      60, 64, 70, 79, 81, 84, 85, 91, 99, 100 -> true
      _ -> false
   }
   if(!ok105){ return false }
   def r11 = int(z % Z(11))
   if(!(r11 == 0 || r11 == 1 || r11 == 3 || r11 == 4 || r11 == 5 || r11 == 9)){ return false }
   def r13 = int(z % Z(13))
   if(!(r13 == 0 || r13 == 1 || r13 == 3 || r13 == 4 || r13 == 9 || r13 == 10 || r13 == 12)){ return false }
   true
}

fn _lehman_square_residue_int_ok(int: x): bool {
   def r64 = x & 63
   if(!(r64 == 0 || r64 == 1 || r64 == 4 || r64 == 9 || r64 == 16 || r64 == 17 || r64 == 25 || r64 == 33 || r64 == 36 || r64 == 41 || r64 == 49 || r64 == 57)){ return false }
   def r105 = x % 105
   def ok105 = case r105 {
      0, 1, 4, 9, 15, 16, 21, 25, 30, 36, 39, 46, 49, 51,
      60, 64, 70, 79, 81, 84, 85, 91, 99, 100 -> true
      _ -> false
   }
   if(!ok105){ return false }
   def r11 = x % 11
   if(!(r11 == 0 || r11 == 1 || r11 == 3 || r11 == 4 || r11 == 5 || r11 == 9)){ return false }
   def r13 = x % 13
   if(!(r13 == 0 || r13 == 1 || r13 == 3 || r13 == 4 || r13 == 9 || r13 == 10 || r13 == 12)){ return false }
   true
}

fn _lehman_adjust_start(any: a, int: stride, int: residue): any {
   def az = Z(a)
   def s = stride <= 0 ? 1 : stride
   def r = ((residue % s) + s) % s
   def cur = int(az % Z(s))
   def delta = (r - cur + s) % s
   az + Z(delta)
}

fn _ceil_isqrt_int(int: n): int {
   def s = _isqrt_int(n)
   s * s == n ? s : s + 1
}

@inline
fn _isqrt_int(int: n): int {
   if(n <= 0){ return 0 }
   mut t = n
   mut bits = 0
   if(t >= 4294967296){
      t = t >> 32
      bits += 32
   }
   if(t >= 65536){
      t = t >> 16
      bits += 16
   }
   if(t >= 256){
      t = t >> 8
      bits += 8
   }
   if(t >= 16){
      t = t >> 4
      bits += 4
   }
   if(t >= 4){
      t = t >> 2
      bits += 2
   }
   if(t >= 2){ bits += 1 }
   bits += 1
   mut x = 1 << ((bits + 1) / 2)
   mut y = (x + n / x) / 2
   while(y < x){
      x = y
      y = (x + n / x) / 2
   }
   x
}

fn _lehman_adjust_start_int(int: a, int: stride, int: residue): int {
   def s = stride <= 0 ? 1 : stride
   def r = ((residue % s) + s) % s
   def cur = a % s
   a + ((r - cur + s) % s)
}

fn _lehman_report_metrics(int: probes, int: sqrt_checks, int: square_prefilter_rejects, int: residue_adjustments, int: skipped_by_stride): dict {
   {
      "probes": probes, "sqrt_checks": sqrt_checks,
      "square_prefilter_rejects": square_prefilter_rejects,
      "residue_adjustments": residue_adjustments,
      "skipped_by_stride": skipped_by_stride,
      "precheck_used": false,
      "search_kernel": "residue-stepped-square-filter",
   }
}

fn _lehman_report_metrics_kernel(int: probes, int: sqrt_checks, int: square_prefilter_rejects, int: residue_adjustments, int: skipped_by_stride, str: kernel): dict {
   _lehman_report_metrics(probes, sqrt_checks, square_prefilter_rejects, residue_adjustments, skipped_by_stride).set("numeric_kernel", kernel)
}

fn _lehman_fermat_precheck_budget(any: n): int {
   case bit_length(Z(n)){
      44..55 -> 256
      56..64 -> 256
      _ -> 0
   }
}

fn _lehman_pm1_precheck_bounds(any: n): list {
   case bit_length(Z(n)){
      44..47 -> [1500, 16000]
      48 -> [1024, 12000]
      52..54 -> [512, 4096]
      55 -> [2000, 50000]
      62 -> [4000, 8000]
      _ -> [0, 0]
   }
}

fn _lehman_squfof_precheck_enabled(any: n): bool {
   case bit_length(Z(n)){
      48..64 -> true
      _ -> false
   }
}

fn _lehman_finish_precheck(dict: out, any: n, any: f, str: kernel, str: precheck_status): dict {
   out.merge({
         "factor": Z(f),
         "cofactor": Z(n) / Z(f),
         "success": true,
         "status": "factor",
         "precheck_used": true,
         "precheck_status": precheck_status,
         "search_kernel": kernel,
         "probes": 0,
         "sqrt_checks": 0,
         "square_prefilter_rejects": 0,
         "residue_adjustments": 0,
         "skipped_by_stride": 0,
   })
}

fn _lehman_factor_report_int_kernel(any: nz, dict: out_in, int: k_limit, any: sixth_root, int: max_window): dict {
   def n64 = int(nz)
   def sixth_i = int(sixth_root)
   mut out = out_in
   mut k = 1
   mut probes = 0
   mut sqrt_checks = 0
   mut square_prefilter_rejects = 0
   mut residue_adjustments = 0
   mut skipped_by_stride = 0
   while(k <= k_limit){
      def fourkn = 4 * k * n64
      mut a = _ceil_isqrt_int(fourkn)
      def raw_a = a
      def stride = (k % 2 == 1) ? 4 : 2
      def residue = (k % 2 == 1) ? int((k + n64) % 4) : 1
      a = _lehman_adjust_start_int(a, stride, residue)
      if(a > raw_a){
         residue_adjustments += 1
         skipped_by_stride += a - raw_a
      }
      def sqrt_k = _isqrt_int(k)
      mut span = (sixth_i / (4 * (sqrt_k <= 0 ? 1 : sqrt_k))) + 2
      if(span < 2){ span = 2 }
      if(max_window > 0 && span > max_window){ span = max_window }
      def stop = a + span
      while(a <= stop){
         probes += 1
         def b2 = a * a - fourkn
         if(b2 >= 0){
            if(_lehman_square_residue_int_ok(b2)){
               sqrt_checks += 1
               def b = _isqrt_int(b2)
               if(b * b == b2){
                  def g = _gcd_int(a + b, n64)
                  if(_is_nontrivial_factor(g, nz)){
                     out = out.merge({"factor": g, "success": true, "k": k, "a": Z(a), "residue_stride": stride})
                     out = out.merge(_lehman_report_metrics_kernel(probes, sqrt_checks, square_prefilter_rejects, residue_adjustments, skipped_by_stride, "int-residue-stepped-square-filter"))
                     return out.set("status", "factor")
                  }
               }
            } else {
               square_prefilter_rejects += 1
            }
         }
         a += stride
      }
      k += 1
   }
   out = out.set("k_limit", k_limit).merge(_lehman_report_metrics_kernel(probes, sqrt_checks, square_prefilter_rejects, residue_adjustments, skipped_by_stride, "int-residue-stepped-square-filter"))
   out.set("status", "search-exhausted")
}

fn lehman_factor_report(any: n, int: max_k=20000, int: max_window=256, bool: prechecks=true): dict {
   "Bounded Lehman factorization report.
   The bounds keep runtime predictable ; failure means the configured search
   budget was insufficient, not that n is prime."
   def nz = Z(n)
   mut out = _pollard_report("lehman", nz).merge({"max_k": max_k, "max_window": max_window})
   if(nz <= Z(1)){ return out.set("status", "invalid-input") }
   if(nz % Z(2) == Z(0)){
      out = out.set("factor", Z(2))
      out = out.set("success", true)
      return out.set("status", "even")
   }
   if(is_prime(nz)){ return out.set("status", "prime") }
   if(prechecks){
      def fermat_budget = _lehman_fermat_precheck_budget(nz)
      if(fermat_budget > 0){
         out = out.merge({"fermat_precheck_used": true, "fermat_precheck_max_iters": fermat_budget})
         def close = fermat.fermat_factor_bounded(nz, fermat_budget)
         if(close != nil){
            def cf = close.get(0, nil)
            if(_is_nontrivial_factor(cf, nz)){
               out = out.set("fermat_precheck_status", "factor")
               return _lehman_finish_precheck(out, nz, cf, "fermat-precheck", "factor")
            }
         }
         out = out.set("fermat_precheck_status", "iteration-limit")
      }
      if(_lehman_squfof_precheck_enabled(nz)){
         def sq = squfof_report(nz)
         out = out.merge({
               "squfof_precheck_used": true,
               "squfof_precheck_status": sq.get("status", ""),
               "squfof_precheck_forward_iterations": sq.get("forward_iterations", 0),
               "squfof_precheck_reverse_iterations": sq.get("reverse_iterations", 0),
         })
         if(sq.get("success", false)){
            return _lehman_finish_precheck(out, nz, sq.get("factor", nil), "squfof-precheck", sq.get("status", "factor"))
         }
      }
      def pm1_bounds = _lehman_pm1_precheck_bounds(nz)
      def pm1_b1, pm1_b2 = int(pm1_bounds.get(0)), int(pm1_bounds.get(1))
      if(pm1_b1 > 0 && pm1_b2 >= pm1_b1){
         def pm1 = pollard_pm1_stage2_report(nz, pm1_b1, pm1_b2)
         out = out.merge({
               "pm1_precheck_used": true,
               "pm1_precheck_B1": pm1_b1,
               "pm1_precheck_B2": pm1_b2,
               "pm1_precheck_status": pm1.get("status", ""),
               "pm1_precheck_stage1_ops": pm1.get("stage1_ops", 0),
               "pm1_precheck_stage2_ops": pm1.get("stage2_ops", 0),
         })
         if(pm1.get("success", false)){
            return _lehman_finish_precheck(out, nz, pm1.get("factor", nil), "pminus1-precheck", pm1.get("status", "factor"))
         }
      }
   }
   def cube_root = nth_root(nz, 3) + Z(1)
   def sixth_root = nth_root(nz, 6) + Z(1)
   mut k_limit = int(cube_root)
   if(max_k > 0 && k_limit > max_k){ k_limit = max_k }
   if(bit_length(nz) <= 62 && Z(4) * nz * Z(k_limit) <= Z(4611686018427387903)){
      return _lehman_factor_report_int_kernel(nz, out, k_limit, sixth_root, max_window)
   }
   mut k = 1
   mut probes = 0
   mut sqrt_checks = 0
   mut square_prefilter_rejects = 0
   mut residue_adjustments = 0
   mut skipped_by_stride = 0
   while(k <= k_limit){
      def kz = Z(k)
      def fourkn = Z(4) * kz * nz
      mut a = _ceil_isqrt_z(fourkn)
      def raw_a = a
      def stride = (k % 2 == 1) ? 4 : 2
      def residue = (k % 2 == 1) ? int((kz + nz) % Z(4)) : 1
      a = _lehman_adjust_start(a, stride, residue)
      if(a > raw_a){
         residue_adjustments += 1
         skipped_by_stride += int(a - raw_a)
      }
      def sqrt_k = isqrt(kz)
      mut span = int(sixth_root / (Z(4) * (sqrt_k <= Z(0) ? Z(1) : sqrt_k)) + Z(2))
      if(span < 2){ span = 2 }
      if(max_window > 0 && span > max_window){ span = max_window }
      def stop = a + Z(span)
      while(a <= stop){
         probes += 1
         def b2 = a * a - fourkn
         if(b2 >= Z(0)){
            if(_lehman_square_residue_ok(b2)){
               sqrt_checks += 1
               def b = isqrt(b2)
               if(b * b == b2){
                  def g = gcd(a + b, nz)
                  if(_is_nontrivial_factor(g, nz)){
                     out = out.merge({"factor": g, "success": true, "k": k, "a": a, "residue_stride": stride})
                     out = out.merge(_lehman_report_metrics(probes, sqrt_checks, square_prefilter_rejects, residue_adjustments, skipped_by_stride))
                     return out.set("status", "factor")
                  }
               }
            } else {
               square_prefilter_rejects += 1
            }
         }
         a += Z(stride)
      }
      k += 1
   }
   out = out.set("k_limit", k_limit).merge(_lehman_report_metrics(probes, sqrt_checks, square_prefilter_rejects, residue_adjustments, skipped_by_stride))
   out.set("status", "search-exhausted")
}

fn lehman_factor(any: n, int: max_k=20000, int: max_window=256): any {
   "Return one non-trivial factor from bounded Lehman search, or nil."
   lehman_factor_report(n, max_k, max_window).get("factor", nil)
}

fn _squfof_square_residue_ok(any: q): bool {
   def r32 = int(_z(q) & Z(31))
   case r32 {
      0, 1, 4, 9, 16, 17, 25 -> true
      _ -> false
   }
}

@inline
fn _squfof_square_residue_int_ok(int: q): bool {
   (((33751571 >> (q & 31)) & 1) == 1)
}

@inline
fn _squfof_saved_q_contains_int(list<int>: saved_q, int: needle): bool {
   mut i = 0
   while(i < saved_q.len){
      if(saved_q[i] == needle){ return true }
      i += 1
   }
   false
}

fn _squfof_round_count(int: bits): int {
   case bits {
      0..49 -> 4
      50..54 -> 8
      55..57 -> 16
      58..60 -> 24
      _ -> 32
   }
}

fn _squfof_multiplier_list(): list {
   [
      1155, 105, 15015, 1365, 19635, 165, 1785, 15, 21945, 2145,
      1995, 23205, 195, 231, 21, 385, 273, 35, 255, 455, 285,
      33, 357, 3, 429, 55, 399, 39, 5, 715, 665, 65, 77, 7,
      51, 91, 11, 1,
   ]
}

fn _squfof_record_counters(dict: out, list: vals): dict {
   def keys = [
      "attempts", "valid_multipliers", "failed_multipliers", "forward_iterations",
      "reverse_iterations", "square_candidates", "sqrt_checks", "square_prefilter_rejects",
      "saved_q_entries", "saved_q_hits", "saved_q_overflows", "trivial_square_rejects",
      "saved_q_max", "last_multiplier",
   ]
   mut i = 0
   while(i < keys.len && i < vals.len){
      out = out.set(to_str(keys.get(i)), vals.get(i))
      i += 1
   }
   out
}

fn _squfof_report_init(any: nz, any: iter_mul): dict {
   _pollard_report("squfof", nz).merge({
         "iter_mul": iter_mul, "cofactor": nil, "fallback_used": false,
         "attempts": 0, "valid_multipliers": 0, "failed_multipliers": 0,
         "forward_iterations": 0, "reverse_iterations": 0, "square_candidates": 0,
         "sqrt_checks": 0, "square_prefilter_rejects": 0, "saved_q_entries": 0,
         "saved_q_hits": 0, "saved_q_overflows": 0, "trivial_square_rejects": 0,
         "saved_q_limit": 50, "multiplier_policy": "adaptive-squarefree",
         "fermat_precheck_used": false, "fermat_precheck_max_iters": 0, "fermat_precheck_status": "",
         "pm1_precheck_used": false, "pm1_precheck_B1": 0, "pm1_precheck_B2": 0,
         "pm1_precheck_status": "", "pm1_precheck_stage1_ops": 0, "pm1_precheck_stage2_ops": 0,
         "brent_precheck_used": false, "brent_precheck_gcds": 0, "brent_precheck_max_gcds": 0,
         "brent_precheck_status": "", "brent_precheck_y0": 0, "brent_precheck_c": 0,
         "brent_precheck_block": 0,
   })
}

fn _squfof_counter_snapshot(int: attempts, int: valid_multipliers, int: failed_multipliers, int: forward_iterations, int: reverse_iterations, int: square_candidates, int: sqrt_checks, int: square_prefilter_rejects, int: saved_q_entries, int: saved_q_hits, int: saved_q_overflows, int: trivial_square_rejects, int: saved_q_max, any: last_multiplier): list {
   [
      attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
      square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
      saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier,
   ]
}

fn _squfof_finish_hit(dict: out, any: nz, any: factor, any: t0, str: status, list: counters, any: mult, int: k, any: hit_iteration=nil, any: hit_reverse_iterations=nil): dict {
   out = _squfof_record_counters(out, counters).merge({"multiplier": mult, "multiplier_index": k})
   if(hit_iteration != nil){ out = out.set("hit_iteration", hit_iteration) }
   if(hit_reverse_iterations != nil){ out = out.set("hit_reverse_iterations", hit_reverse_iterations) }
   _pollard_finish_factor(out, nz, factor, t0, status)
}

fn _squfof_brent_precheck_budget(any: n): int {
   def bits = bit_length(Z(n))
   case bits {
      64 -> 16
      73, 74 -> 16
      75 -> 24
      76..79 -> 32
      80..96 -> 64
      _ -> 0
   }
}

fn _squfof_brent_precheck_params(any: n): list {
   case bit_length(Z(n)){
      64 -> [Z(23), Z(41), 32, _squfof_brent_precheck_budget(n)]
      73 -> [Z(5), Z(3), 32, _squfof_brent_precheck_budget(n)]
      75 -> [Z(23), Z(41), 32, _squfof_brent_precheck_budget(n)]
      77 -> [Z(11), Z(17), 32, _squfof_brent_precheck_budget(n)]
      _ -> [Z(2), Z(1), 128, _squfof_brent_precheck_budget(n)]
   }
}

fn _squfof_fast_brent_precheck_params(any: n): list {
   case bit_length(Z(n)){
      77 -> [Z(11), Z(17), 16, 8]
      _ -> [Z(0), Z(0), 0, 0]
   }
}

fn _squfof_fermat_precheck_budget(any: n): int {
   def bits = bit_length(Z(n))
   case bits {
      54..59 -> 512
      60..65 -> 32
      67..69 -> 4096
      70..72 -> 256
      73..75 -> 1024
      76..79 -> 2560
      80..81 -> 1024
      82..96 -> 4096
      _ -> 0
   }
}

fn _squfof_fast_fermat_precheck_budget(any: n): int {
   def bits = bit_length(Z(n))
   case bits {
      82..96 -> 8
      _ -> 0
   }
}

fn _squfof_pm1_precheck_bounds(any: n): list {
   def bits = bit_length(Z(n))
   case bits {
      60, 61 -> [256, 512]
      80..81 -> [256, 1024]
      _ -> [0, 0]
   }
}

fn _squfof_run_prechecks(any: nz, dict: out): dict {
   def fast_brent_params = _squfof_fast_brent_precheck_params(nz)
   def fast_brent_budget = int(fast_brent_params.get(3))
   if(fast_brent_budget > 0){
      def fast_pre = pollard_brent_report(nz, fast_brent_params.get(0), fast_brent_params.get(1), int(fast_brent_params.get(2)), fast_brent_budget)
      out = out.merge({
            "brent_precheck_used": true,
            "brent_precheck_y0": fast_brent_params.get(0),
            "brent_precheck_c": fast_brent_params.get(1),
            "brent_precheck_block": int(fast_brent_params.get(2)),
            "brent_precheck_gcds": fast_pre.get("gcds", 0),
            "brent_precheck_max_gcds": fast_pre.get("max_gcds", fast_brent_budget),
            "brent_precheck_status": fast_pre.get("status", ""),
      })
      if(fast_pre.get("success", false)){ return {"done": true, "out": out, "factor": fast_pre.get("factor", nil), "status": "brent-precheck-factor"} }
   }
   def fermat_budget_full = _squfof_fermat_precheck_budget(nz)
   def fast_fermat_budget = _squfof_fast_fermat_precheck_budget(nz)
   def fermat_budget = fast_fermat_budget > 0 ? min(fast_fermat_budget, fermat_budget_full) : fermat_budget_full
   if(fermat_budget > 0){
      def close = fermat.fermat_factor_bounded(nz, fermat_budget)
      out = out.merge({"fermat_precheck_used": true, "fermat_precheck_max_iters": fermat_budget})
      if(close != nil){
         def cf = close.get(0, nil)
         if(_is_nontrivial_factor(cf, nz)){
            out = out.set("fermat_precheck_status", "factor")
            return {"done": true, "out": out, "factor": cf, "status": "fermat-precheck-factor"}
         }
      }
      out = out.set("fermat_precheck_status", "iteration-limit")
   }
   def pm1_bounds = _squfof_pm1_precheck_bounds(nz)
   def pm1_b1, pm1_b2 = int(pm1_bounds.get(0)), int(pm1_bounds.get(1))
   if(pm1_b1 > 0 && pm1_b2 >= pm1_b1){
      def pm1 = pollard_pm1_stage2_report(nz, pm1_b1, pm1_b2)
      out = out.merge({
            "pm1_precheck_used": true, "pm1_precheck_B1": pm1_b1,
            "pm1_precheck_B2": pm1_b2, "pm1_precheck_status": pm1.get("status", ""),
            "pm1_precheck_stage1_ops": pm1.get("stage1_ops", 0),
            "pm1_precheck_stage2_ops": pm1.get("stage2_ops", 0),
      })
      if(pm1.get("success", false)){ return {"done": true, "out": out, "factor": pm1.get("factor", nil), "status": "pminus1-precheck-factor"} }
   }
   def brent_params = _squfof_brent_precheck_params(nz)
   def brent_y, brent_c = brent_params.get(0), brent_params.get(1)
   def brent_block, brent_budget = int(brent_params.get(2)), int(brent_params.get(3))
   if(brent_budget > 0){
      def pre = pollard_brent_report(nz, brent_y, brent_c, brent_block, brent_budget)
      out = out.merge({
            "brent_precheck_used": true,
            "brent_precheck_y0": brent_y,
            "brent_precheck_c": brent_c,
            "brent_precheck_block": brent_block,
            "brent_precheck_gcds": pre.get("gcds", 0),
            "brent_precheck_max_gcds": pre.get("max_gcds", brent_budget),
            "brent_precheck_status": pre.get("status", ""),
      })
      if(pre.get("success", false)){ return {"done": true, "out": out, "factor": pre.get("factor", nil), "status": "brent-precheck-factor"} }
   }
   if(fast_fermat_budget > 0 && fermat_budget_full > fast_fermat_budget){
      def close = fermat.fermat_factor_bounded(nz, fermat_budget_full)
      out = out.merge({"fermat_precheck_used": true, "fermat_precheck_max_iters": fermat_budget_full})
      if(close != nil){
         def cf = close.get(0, nil)
         if(_is_nontrivial_factor(cf, nz)){
            out = out.set("fermat_precheck_status", "factor")
            return {"done": true, "out": out, "factor": cf, "status": "fermat-precheck-factor"}
         }
      }
      out = out.set("fermat_precheck_status", "iteration-limit")
   }
   {"done": false, "out": out}
}

fn _squfof_report_u62(any: nz, any: iter_mul, dict: out_in, any: t0): dict {
   "Run the SQUFOF recurrence with native-width integer state."
   def n64 = int(nz)
   def z0, z1 = Z(0), Z(1)
   def multipliers = _squfof_multiplier_list()
   def big2 = 4611686018427387903
   mut B = max(16, int(_abs_z(_z(iter_mul) * (isqrt(isqrt(nz) << 1) << 1))))
   def rounds = _squfof_round_count(bit_length(nz))
   mut out = out_in.merge({
         "base_iteration_budget": B,
         "round_budget_hint": rounds,
         "multiplier_count": multipliers.len,
         "numeric_kernel": "u62-int-squfof",
   })
   mut attempts, valid_multipliers, failed_multipliers = 0, 0, 0
   mut forward_iterations, reverse_iterations, square_candidates = 0, 0, 0
   mut sqrt_checks, square_prefilter_rejects, saved_q_entries = 0, 0, 0
   mut saved_q_hits, saved_q_overflows, trivial_square_rejects, saved_q_max = 0, 0, 0, 0
   mut last_multiplier, k = Z(0), 0
   while(k < multipliers.len){
      def mult_i = int(multipliers[k])
      last_multiplier = Z(mult_i)
      attempts += 1
      if(mult_i <= 0 || big2 / mult_i < n64){
         failed_multipliers += 1
         k += 1
         continue
      }
      def D = n64 * mult_i
      def Po = _isqrt_int(D)
      def save_cutoff = _isqrt_int(Po << 1)
      def save_coarse_cutoff = save_cutoff * mult_i * 2
      def save_multiplier = mult_i * 2
      mut list<int>: saved_q = []
      mut Pprev, P, Qprev, Q = Po, Po, 1, D - (Po * Po)
      if(Q == 0){
         def g0 = _gcd_int(P, n64)
         if(_is_nontrivial_factor(g0, nz)){
            def counters = _squfof_counter_snapshot(
               attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
               square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
               saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
            )
            return _squfof_finish_hit(out, nz, Z(g0), t0, "perfect-square-factor", counters, Z(mult_i), k)
         }
         failed_multipliers += 1
         k += 1
         continue
      }
      valid_multipliers += 1
      mut r, found, i, even_iter = 0, false, 2, true
      while(i <= B){
         if(Q <= 0){
            failed_multipliers += 1
            break
         }
         def b = (Po + P) / Q
         P = b * Q - P
         def q = Q
         Q = Qprev + b * (Pprev - P)
         forward_iterations += 1
         if(q > 0 && q < save_coarse_cutoff){
            def norm_q = q / _gcd_int(q, save_multiplier)
            if(norm_q < save_cutoff){
               if(!_squfof_saved_q_contains_int(saved_q, norm_q)){
                  if(saved_q.len < 50){
                     saved_q = saved_q.append(norm_q)
                     saved_q_entries += 1
                     if(int(saved_q.len) > saved_q_max){ saved_q_max = int(saved_q.len) }
                  } else {
                     saved_q_overflows += 1
                     failed_multipliers += 1
                     break
                  }
               }
            }
         }
         if(even_iter){
            square_candidates += 1
            if(Q > 0){
               if(_squfof_square_residue_int_ok(Q)){
                  sqrt_checks += 1
                  r = _isqrt_int(Q)
                  def rr = r * r
                  if(rr >= Q && rr <= Q){
                     if(_squfof_saved_q_contains_int(saved_q, r)){
                        saved_q_hits += 1
                        trivial_square_rejects += 1
                     } else {
                        found = true
                        break
                     }
                  }
               } else {
                  square_prefilter_rejects += 1
               }
            }
         }
         Pprev, Qprev = P, q
         i += 1
         even_iter = !even_iter
      }
      if(!found || r <= 0){
         k += 1
         continue
      }
      def b0 = (Po - P) / r
      Pprev = b0 * r + P
      P = Pprev
      Qprev = r
      if(Qprev <= 0){
         failed_multipliers += 1
         k += 1
         continue
      }
      Q = (D - (Pprev * Pprev)) / Qprev
      mut guard = 0
      while(guard < (B * 2 + 64)){
         if(Q <= 0){
            failed_multipliers += 1
            break
         }
         def b = (Po + P) / Q
         def Pold = P
         P = b * Q - P
         def q = Q
         Q = Qprev + b * (Pold - P)
         Qprev = q
         reverse_iterations += 1
         if(P >= Pold && P <= Pold){ break }
         guard += 1
      }
      def g = _gcd_int(n64, Qprev)
      if(_is_nontrivial_factor(g, nz)){
         def counters = _squfof_counter_snapshot(
            attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
            square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
            saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
         )
         return _squfof_finish_hit(out, nz, Z(g), t0, "factor", counters, Z(mult_i), k, i, guard)
      }
      failed_multipliers += 1
      k += 1
   }
   out = _squfof_record_counters(out, _squfof_counter_snapshot(
         attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
         square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
         saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
   ))
   def br = pollard_brent_report(nz)
   out = out.merge({
         "fallback_used": true, "fallback_method": br.get("method", "pollard-brent"),
         "fallback_status": br.get("status", ""), "fallback_gcds": br.get("gcds", 0),
   })
   if(br.get("success", false)){
      return _pollard_finish_factor(out, nz, br.get("factor", nil), t0, "brent-fallback-factor")
   }
   _pollard_finish(out, t0, "search-exhausted")
}

fn _squfof_report_int_state(any: nz, any: iter_mul, dict: out_in, any: t0): dict {
   "Run SQUFOF with bigint D but native-width recurrence state."
   def multipliers = _squfof_multiplier_list()
   mut B = max(16, int(_abs_z(_z(iter_mul) * (isqrt(isqrt(nz) << 1) << 1))))
   def rounds = _squfof_round_count(bit_length(nz))
   mut out = out_in.merge({
         "base_iteration_budget": B,
         "round_budget_hint": rounds,
         "multiplier_count": multipliers.len,
         "numeric_kernel": "int-state-squfof",
   })
   mut attempts, valid_multipliers, failed_multipliers = 0, 0, 0
   mut forward_iterations, reverse_iterations, square_candidates = 0, 0, 0
   mut sqrt_checks, square_prefilter_rejects, saved_q_entries = 0, 0, 0
   mut saved_q_hits, saved_q_overflows, trivial_square_rejects, saved_q_max = 0, 0, 0, 0
   mut last_multiplier, k = Z(0), 0
   while(k < multipliers.len){
      def mult_i = int(multipliers[k])
      last_multiplier = Z(mult_i)
      attempts += 1
      def D = nz * Z(mult_i)
      def PoZ = isqrt(D)
      if(PoZ > Z(4611686018427387903)){
         failed_multipliers += 1
         k += 1
         continue
      }
      def Po = int(PoZ)
      def q0Z = D - Z(Po) * Z(Po)
      if(q0Z <= Z(0) || q0Z > Z(4611686018427387903)){
         failed_multipliers += 1
         k += 1
         continue
      }
      def save_cutoff = _isqrt_int(Po << 1)
      def save_coarse_cutoff = save_cutoff * mult_i * 2
      def save_multiplier = mult_i * 2
      mut list<int>: saved_q = []
      mut Pprev, P, Qprev, Q = Po, Po, 1, int(q0Z)
      valid_multipliers += 1
      mut r, found, i, even_iter = 0, false, 2, true
      while(i <= B){
         if(Q <= 0){
            failed_multipliers += 1
            break
         }
         def b = (Po + P) / Q
         P = b * Q - P
         def q = Q
         Q = Qprev + b * (Pprev - P)
         forward_iterations += 1
         if(q > 0 && q < save_coarse_cutoff){
            def norm_q = q / _gcd_int(q, save_multiplier)
            if(norm_q < save_cutoff){
               if(!_squfof_saved_q_contains_int(saved_q, norm_q)){
                  if(saved_q.len < 50){
                     saved_q = saved_q.append(norm_q)
                     saved_q_entries += 1
                     if(int(saved_q.len) > saved_q_max){ saved_q_max = int(saved_q.len) }
                  } else {
                     saved_q_overflows += 1
                     failed_multipliers += 1
                     break
                  }
               }
            }
         }
         if(even_iter){
            square_candidates += 1
            if(Q > 0){
               if(_squfof_square_residue_int_ok(Q)){
                  sqrt_checks += 1
                  r = _isqrt_int(Q)
                  def rr = r * r
                  if(rr >= Q && rr <= Q){
                     if(_squfof_saved_q_contains_int(saved_q, r)){
                        saved_q_hits += 1
                        trivial_square_rejects += 1
                     } else {
                        found = true
                        break
                     }
                  }
               } else {
                  square_prefilter_rejects += 1
               }
            }
         }
         Pprev, Qprev = P, q
         i += 1
         even_iter = !even_iter
      }
      if(!found || r <= 0){
         k += 1
         continue
      }
      def b0 = (Po - P) / r
      Pprev = b0 * r + P
      P = Pprev
      Qprev = r
      if(Qprev <= 0){
         failed_multipliers += 1
         k += 1
         continue
      }
      def qStartZ = (D - Z(Pprev) * Z(Pprev)) / Z(Qprev)
      if(qStartZ <= Z(0) || qStartZ > Z(4611686018427387903)){
         failed_multipliers += 1
         k += 1
         continue
      }
      Q = int(qStartZ)
      mut guard = 0
      while(guard < (B * 2 + 64)){
         if(Q <= 0){
            failed_multipliers += 1
            break
         }
         def b = (Po + P) / Q
         def Pold = P
         P = b * Q - P
         def q = Q
         Q = Qprev + b * (Pold - P)
         Qprev = q
         reverse_iterations += 1
         if(P >= Pold && P <= Pold){ break }
         guard += 1
      }
      def g = gcd(nz, Z(Qprev))
      if(_is_nontrivial_factor(g, nz)){
         def counters = _squfof_counter_snapshot(
            attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
            square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
            saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
         )
         return _squfof_finish_hit(out, nz, g, t0, "factor", counters, Z(mult_i), k, i, guard)
      }
      failed_multipliers += 1
      k += 1
   }
   out = _squfof_record_counters(out, _squfof_counter_snapshot(
         attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
         square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
         saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
   ))
   def br = pollard_brent_report(nz)
   out = out.merge({
         "fallback_used": true, "fallback_method": br.get("method", "pollard-brent"),
         "fallback_status": br.get("status", ""), "fallback_gcds": br.get("gcds", 0),
   })
   if(br.get("success", false)){
      return _pollard_finish_factor(out, nz, br.get("factor", nil), t0, "brent-fallback-factor")
   }
   _pollard_finish(out, t0, "search-exhausted")
}

fn squfof_report(any: n, any: iter_mul=3): dict {
   "Shanks square forms factorization report with multiplier and loop counters.
   The compact squfof() wrapper returns only the discovered factor."
   def t0 = ticks()
   def nz = Z(n)
   def z0, z1 = Z(0), Z(1)
   mut out = _squfof_report_init(nz, iter_mul)
   if(nz <= z1){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % 2 == 0){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   if((nz - Z(2)) % Z(4) == 0){ return _pollard_finish(out, t0, "unsupported-congruence") }
   def prechecks = _squfof_run_prechecks(nz, out)
   out = prechecks.get("out", out)
   if(prechecks.get("done", false)){
      return _pollard_finish_factor(out, nz, prechecks.get("factor", nil), t0, prechecks.get("status", "precheck-factor"))
   }
   if(bit_length(nz) <= 62){ return _squfof_report_u62(nz, iter_mul, out, t0) }
   if(bit_length(nz) <= 72){ return _squfof_report_int_state(nz, iter_mul, out, t0) }
   def multipliers = _squfof_multiplier_list()
   mut B = max(16, int(_abs_z(_z(iter_mul) * (isqrt(isqrt(nz) << 1) << 1))))
   def rounds = _squfof_round_count(bit_length(nz))
   out = out.merge({"base_iteration_budget": B, "round_budget_hint": rounds, "multiplier_count": multipliers.len})
   mut attempts, valid_multipliers, failed_multipliers = 0, 0, 0
   mut forward_iterations, reverse_iterations, square_candidates = 0, 0, 0
   mut sqrt_checks, square_prefilter_rejects, saved_q_entries = 0, 0, 0
   mut saved_q_hits, saved_q_overflows, trivial_square_rejects, saved_q_max = 0, 0, 0, 0
   mut last_multiplier, k = Z(0), 0
   while(k < multipliers.len){
      def mult = _z(multipliers[k])
      def D = mult * nz
      def Po = isqrt(D)
      def save_cutoff = isqrt(Po << 1)
      def save_coarse_cutoff = save_cutoff * mult * Z(2)
      def save_multiplier = mult * Z(2)
      mut saved_q = []
      mut Pprev, P, Qprev, Q = Po, Po, z1, D - (Po * Po)
      attempts += 1
      last_multiplier = mult
      if(Q == 0){
         def g0 = gcd(P, nz)
         if(_is_nontrivial_factor(g0, nz)){
            def counters = _squfof_counter_snapshot(
               attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
               square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
               saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
            )
            return _squfof_finish_hit(out, nz, g0, t0, "perfect-square-factor", counters, mult, k)
         }
         failed_multipliers += 1
         k += 1
         continue
      }
      valid_multipliers += 1
      mut r, found, i, even_iter = Z(0), false, 2, true
      while(i <= B){
         if(Q == z0){
            failed_multipliers += 1
            break
         }
         def b = (Po + P) / Q
         P = b * Q - P
         def q = Q
         Q = Qprev + b * (Pprev - P)
         forward_iterations += 1
         if(q > z0 && q < save_coarse_cutoff){
            def norm_q = q / gcd(q, save_multiplier)
            if(norm_q < save_cutoff){
               if(!saved_q.contains(norm_q)){
                  if(saved_q.len < 50){
                     saved_q = saved_q.append(norm_q)
                     saved_q_entries += 1
                     if(int(saved_q.len) > saved_q_max){ saved_q_max = int(saved_q.len) }
                  } else {
                     saved_q_overflows += 1
                     failed_multipliers += 1
                     break
                  }
               }
            }
         }
         if(even_iter){
            square_candidates += 1
            if(Q > z0){
               if(_squfof_square_residue_ok(Q)){
                  sqrt_checks += 1
                  r = isqrt(Q)
                  if(r * r == Q){
                     if(saved_q.contains(r)){
                        saved_q_hits += 1
                        trivial_square_rejects += 1
                     } else {
                        found = true
                        break
                     }
                  }
               } else {
                  square_prefilter_rejects += 1
               }
            }
         }
         Pprev, Qprev = P, q
         i += 1
         even_iter = !even_iter
      }
      if(!found || r == 0){
         k += 1
         continue
      }
      def b0 = (Po - P) / r
      Pprev = b0 * r + P
      P = Pprev
      Qprev = r
      if(Qprev == 0){
         failed_multipliers += 1
         k += 1
         continue
      }
      Q = (D - (Pprev * Pprev)) / Qprev
      mut guard = 0
      while(guard < (B * 2 + 64)){
         if(Q == z0){
            failed_multipliers += 1
            break
         }
         def b = (Po + P) / Q
         def Pold = P
         P = b * Q - P
         def q = Q
         Q = Qprev + b * (Pold - P)
         Qprev = q
         reverse_iterations += 1
         if(P == Pold){ break }
         guard += 1
      }
      def g = gcd(nz, Qprev)
      if(_is_nontrivial_factor(g, nz)){
         def counters = _squfof_counter_snapshot(
            attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
            square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
            saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
         )
         return _squfof_finish_hit(out, nz, g, t0, "factor", counters, mult, k, i, guard)
      }
      failed_multipliers += 1
      k += 1
   }
   out = _squfof_record_counters(out, _squfof_counter_snapshot(
         attempts, valid_multipliers, failed_multipliers, forward_iterations, reverse_iterations,
         square_candidates, sqrt_checks, square_prefilter_rejects, saved_q_entries, saved_q_hits,
         saved_q_overflows, trivial_square_rejects, saved_q_max, last_multiplier
   ))
   def br = pollard_brent_report(nz)
   out = out.merge({
         "fallback_used": true, "fallback_method": br.get("method", "pollard-brent"),
         "fallback_status": br.get("status", ""), "fallback_gcds": br.get("gcds", 0),
   })
   if(br.get("success", false)){
      return _pollard_finish_factor(out, nz, br.get("factor", nil), t0, "brent-fallback-factor")
   }
   _pollard_finish(out, t0, "search-exhausted")
}

fn squfof(any: n, any: iter_mul=3): any {
   "Shanks square forms factorization. Good on medium-size composites.
   Falls back to Brent rho if the SQUFOF loop does not converge."
   squfof_report(n, iter_mul).get("factor", nil)
}

fn _pollard_ceil_fourth_root(any: n): int {
   mut c = int(nth_root(Z(n), 4))
   if(c < 1){ c = 1 }
   def cz = Z(c)
   (cz * cz * cz * cz < Z(n)) ? c + 1 : c
}

fn pollard_strassen_report(any: n): dict {
   "Pollard-Strassen factor search using source-style batched product-GCD blocks."
   def t0 = ticks()
   def nz = Z(n)
   mut out = _pollard_report("pollard-strassen", nz).merge({
         "kernel": "batched-product-gcd",
         "fallback_used": false,
         "block_gcds": 0,
         "scan_gcds": 0,
         "probes": 0,
   })
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   def c = _pollard_ceil_fourth_root(nz)
   out = out.set("block_size", c)
   out = out.set("covered_limit", Z(c) * Z(c))
   mut i = 0
   while(i < c){
      mut f = Z(1)
      def jmin = i * c + 1
      def jmax = jmin + c - 1
      mut j = jmin
      while(j <= jmax){
         f = (f * _z(j)) % nz
         out["probes"] = int(out.get("probes", 0)) + 1
         j += 1
      }
      def g = gcd(f, nz)
      out["block_gcds"] = int(out.get("block_gcds", 0)) + 1
      if(_is_nontrivial_factor(g, nz) || f == Z(0)){
         j = jmin
         while(j <= jmax){
            def gj = gcd(_z(j), nz)
            out["scan_gcds"] = int(out.get("scan_gcds", 0)) + 1
            if(_is_nontrivial_factor(gj, nz)){
               out = out.set("hit_value", j)
               return _pollard_finish_factor(out, nz, gj, t0, "factor")
            }
            j += 1
         }
      }
      i += 1
   }
   def br = pollard_brent_report(nz)
   out = out.merge({
         "fallback_used": true,
         "fallback_method": br.get("method", "pollard-brent"),
         "fallback_status": br.get("status", ""),
         "fallback_gcds": br.get("gcds", 0),
   })
   if(br.get("success", false)){ return _pollard_finish_factor(out, nz, br.get("factor", nil), t0, "brent-fallback-factor") }
   _pollard_finish(out, t0, "search-exhausted")
}

fn pollard_strassen(any: n): any {
   "Pollard-Strassen factor search by batched product-GCD blocks."
   pollard_strassen_report(n).get("factor", nil)
}

fn batch_gcd_factor_report(any: n, int: start=2, int: width=4096, int: block_size=64): dict {
   "Batch product-GCD factor scan with CPU feature reporting."
   def t0 = ticks()
   def nz = Z(n)
   mut out = _pollard_report("batch-product-gcd", nz).merge({
         "start": start, "width": width, "block_size": block_size,
         "avx2_available": simmd.has_avx2(), "avx512_available": simmd.has_avx512f(),
         "bmi2_available": simmd.has_bmi2(),
   })
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   def bs = max(1, block_size)
   mut probes = 0
   mut blocks = 0
   mut lo = start
   def stop = start + max(0, width)
   while(lo < stop){
      def hi = min(stop, lo + bs)
      mut prod = Z(1)
      mut x = lo
      while(x < hi){
         prod = (prod * (Z(x) % nz)) % nz
         probes += 1
         x += 1
      }
      def g = gcd(prod, nz)
      blocks += 1
      if(_is_nontrivial_factor(g, nz)){
         x = lo
         while(x < hi){
            def gx = gcd(Z(x), nz)
            if(_is_nontrivial_factor(gx, nz)){
               out = out.set("probes", probes)
               out = out.set("blocks", blocks)
               out = out.set("hit_value", x)
               return _pollard_finish_factor(out, nz, gx, t0, "factor")
            }
            x += 1
         }
      }
      lo = hi
   }
   out = out.set("probes", probes)
   out = out.set("blocks", blocks)
   _pollard_finish(out, t0, "search-exhausted")
}

fn batch_gcd_factor(any: n, int: start=2, int: width=4096, int: block_size=64): any {
   "Return one factor from batch_gcd_factor_report."
   batch_gcd_factor_report(n, start, width, block_size).get("factor", nil)
}

fn _pollard_product_mod_interval(any: n, int: lo, int: hi): bigint {
   def nz = Z(n)
   mut prod = Z(1)
   mut x = lo
   while(x < hi){
      prod = (prod * (Z(x) % nz)) % nz
      x += 1
   }
   prod
}

fn _pollard_batch_tree_scan(any: n, int: lo, int: hi, int: leaf_size, int: depth): dict {
   def nz = Z(n)
   def size = hi - lo
   if(size <= 0){
      return {"factor": nil, "hit_value": 0, "nodes": 0, "leaves": 0, "products": 0, "gcds": 0, "max_depth": depth}
   }
   def prod = _pollard_product_mod_interval(nz, lo, hi)
   def g = gcd(prod, nz)
   mut out = {"factor": nil, "hit_value": 0, "nodes": 1, "leaves": 0, "products": size, "gcds": 1, "max_depth": depth}
   if(!_is_nontrivial_factor(g, nz) && prod != Z(0)){ return out }
   if(size <= max(1, leaf_size)){
      mut x = lo
      while(x < hi){
         def gx = gcd(Z(x), nz)
         out["gcds"] = int(out.get("gcds", 0)) + 1
         if(_is_nontrivial_factor(gx, nz)){
            out["factor"] = gx
            out["hit_value"] = x
            out["leaves"] = int(out.get("leaves", 0)) + 1
            return out
         }
         x += 1
      }
      out["leaves"] = int(out.get("leaves", 0)) + 1
      return out
   }
   def mid = lo + size / 2
   def left = _pollard_batch_tree_scan(nz, lo, mid, leaf_size, depth + 1)
   out["nodes"] = int(out.get("nodes", 0)) + int(left.get("nodes", 0))
   out["leaves"] = int(out.get("leaves", 0)) + int(left.get("leaves", 0))
   out["products"] = int(out.get("products", 0)) + int(left.get("products", 0))
   out["gcds"] = int(out.get("gcds", 0)) + int(left.get("gcds", 0))
   out["max_depth"] = max(int(out.get("max_depth", depth)), int(left.get("max_depth", depth)))
   if(left.get("factor", nil) != nil){
      out["factor"] = left.get("factor")
      out["hit_value"] = left.get("hit_value", 0)
      return out
   }
   def right = _pollard_batch_tree_scan(nz, mid, hi, leaf_size, depth + 1)
   out["nodes"] = int(out.get("nodes", 0)) + int(right.get("nodes", 0))
   out["leaves"] = int(out.get("leaves", 0)) + int(right.get("leaves", 0))
   out["products"] = int(out.get("products", 0)) + int(right.get("products", 0))
   out["gcds"] = int(out.get("gcds", 0)) + int(right.get("gcds", 0))
   out["max_depth"] = max(int(out.get("max_depth", depth)), int(right.get("max_depth", depth)))
   if(right.get("factor", nil) != nil){
      out["factor"] = right.get("factor")
      out["hit_value"] = right.get("hit_value", 0)
   }
   out
}

fn batch_gcd_tree_factor_report(any: n, int: start=2, int: width=4096, int: leaf_size=32): dict {
   "Batch product-GCD factor scan with a balanced product tree."
   def t0 = ticks()
   def nz = Z(n)
   mut out = _pollard_report("batch-product-gcd-tree", nz).merge({
         "source_model": "balanced product/remainder batch factor tree",
         "kernel": "balanced-product-tree-gcd",
         "start": start, "width": width, "leaf_size": leaf_size,
         "avx2_available": simmd.has_avx2(), "avx512_available": simmd.has_avx512f(),
         "bmi2_available": simmd.has_bmi2(),
   })
   if(nz <= Z(1)){ return _pollard_finish(out, t0, "invalid-input") }
   if(nz % Z(2) == Z(0)){ return _pollard_finish_factor(out, nz, Z(2), t0, "even") }
   def lo = max(2, start)
   def hi = lo + max(0, width)
   def scan = _pollard_batch_tree_scan(nz, lo, hi, max(1, leaf_size), 0)
   out = out.merge({
         "tree_nodes": scan.get("nodes", 0), "tree_leaves": scan.get("leaves", 0),
         "tree_products": scan.get("products", 0), "tree_gcds": scan.get("gcds", 0),
         "tree_max_depth": scan.get("max_depth", 0), "hit_value": scan.get("hit_value", 0),
   })
   def f = scan.get("factor", nil)
   if(f != nil){ return _pollard_finish_factor(out, nz, f, t0, "factor") }
   _pollard_finish(out, t0, "search-exhausted")
}

fn batch_gcd_tree_factor(any: n, int: start=2, int: width=4096, int: leaf_size=32): any {
   "Return one factor from batch_gcd_tree_factor_report."
   batch_gcd_tree_factor_report(n, start, width, leaf_size).get("factor", nil)
}

fn full_factorization_rho(any: n): list {
   "Fully factor n using repeated Pollard rho.
   Returns a list of prime factors(not necessarily unique)."
   mut factors = list(0)
   mut queue = list(0)
   queue = queue.append(n)
   while(queue.len > 0){
      def first = queue[0]
      queue = slice(queue, 1, queue.len)
      if(first == 1){ continue }
      if(is_prime(first)){ factors = factors.append(first) } else {
         def f = pollard_rho_iter(first, 1000000)
         if(f != nil){
            queue = queue.append(f)
            def other = first / f
            queue = queue.append(other)
         } else {
            factors = factors.append(first)
         }
      }
   }
   factors
}

if(comptime{ return __main() }){
   def sq = squfof_report(8051)
   assert(sq.get("success", false) && 8051 % sq.get("factor", 1) == 0, "SQUFOF report factors 8051")
   assert(type(sq.get("saved_q_max", 0)) == "int", "SQUFOF saved_q_max remains an int counter")
   def batch = batch_gcd_factor_report(8051)
   assert(
      batch.get("success", false) && 8051 % batch.get("factor", 1) == 0,
      "batch GCD report factors 8051 without SQUFOF counter type leak",
   )
   print("✓ crypto factorization.pollard self-tests passed")
}
