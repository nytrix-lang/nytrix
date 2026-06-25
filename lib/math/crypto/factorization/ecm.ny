;; Keywords: factorization ecm math crypto number-theory
;; Integer-factorization routines for elliptic-curve factorization method.
;; Reference:
;; - https://en.wikipedia.org/wiki/Lenstra_elliptic-curve_factorization
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.ecm(ecm_factor, ecm_factor_report, micro_ecm_work_plan_report, ecm_work_plan_report, ecm_scheduled_factor_report, ecm_batch_lane_factor_report, ecm_batch_lane_factor, montgomery_ecm_factor, montgomery_ecm_factor_report)
use std.math.nt
use std.os.time (ticks)
use std.math.crypto.factorization.pollard as pollard

mut _ecm_stage1_scalars_cache = dict()
mut _ecm_stage2_scalars_cache = dict()
mut _ecm_stage1_product_cache = dict()
mut _ecm_stage2_product_cache = dict()

fn _ecm_elapsed_ms(any t0) number { float(ticks() - t0) / 1000000.0 }

fn _ecm_z(any x) bigint { is_bigint(x) ? x : Z(x) }

fn _ecm_abs(any x) bigint {
   def z = _ecm_z(x)
   z < Z(0) ? -z : z
}

fn _ecm_nontrivial(any g, any n) bool {
   def gg, nn = _ecm_abs(g), _ecm_abs(n)
   gg > Z(1) && gg < nn && nn % gg == Z(0)
}

fn _ecm_digits(any n) int {
   to_str(_ecm_abs(n)).len
}

fn _ecm_micro_attempt(int B1) dict {
   {"B1": B1, "B2": B1 * 25, "curves": 1, "stage2_ratio": 25}
}

fn _ecm_micro_plan(int bits) list {
   case bits {
      0..40 -> [27, 32, 0]
      41..44 -> [47, 32, 0]
      45..48 -> [70, 32, 0]
      49..52 -> [85, 32, 100]
      53..58 -> [125, 32, 333]
      59..62 -> [165, 42, 333]
      _ -> [205, 42, 333]
   }
}

fn micro_ecm_work_plan_report(any n, bool arbitrary_precheck=false) dict {
   "Return the small-target ECM dispatch plan used for <=64-bit cofactors."
   def t0 = ticks()
   def nn = _ecm_abs(n)
   def bits = bit_length(nn)
   def plan = _ecm_micro_plan(bits)
   def B1, curves, pm1_bound = int(plan.get(0)), int(plan.get(1)), int(plan.get(2))
   mut pre = []
   if arbitrary_precheck {
      pre = pre.append(_ecm_micro_attempt(47))
      pre = pre.append(_ecm_micro_attempt(70))
      if bits > 58 { pre = pre.append(_ecm_micro_attempt(125)) }
   }
   {
      "method": "micro-ecm-work-plan", "n_bits": bits, "digits": _ecm_digits(nn),
      "small_target": bits <= 64, "arbitrary_precheck": arbitrary_precheck,
      "pre_attempts": pre, "pre_pm1_bound": pm1_bound,
      "B1": B1, "B2": B1 * 25, "curves": curves, "stage2_ratio": 25,
      "source": "micro-ecm-dispatch-table",
      "elapsed_ms": _ecm_elapsed_ms(t0),
   }
}

fn ecm_work_plan_report(any n, bool deep=false) dict {
   "Return an ECM work plan: target digits, B1, B2, and curve count."
   def t0 = ticks()
   def nn = _ecm_abs(n)
   def bits = bit_length(nn)
   if bits <= 64 {
      def micro = micro_ecm_work_plan_report(nn, false)
      return micro.merge({"method": "ecm-work-plan", "deep": deep, "selected_plan": "micro-ecm"})
   }
   def digits = _ecm_digits(nn)
   def levels = [15, 20, 25, 30, 35, 40]
   def b1s = [2000, 11000, 50000, 250000, 1000000, 3000000]
   def curves = [30, 74, 214, 430, 904, 2350]
   mut idx = 0
   while idx + 1 < levels.len && digits > levels.get(idx) { idx += 1 }
   if !deep && idx > 0 { idx = 0 }
   def B1 = b1s.get(idx)
   {
      "method": "ecm-work-plan", "n_bits": bits, "digits": digits,
      "deep": deep, "selected_plan": "large-ecm",
      "target_digits": levels.get(idx), "B1": B1, "B2": B1 * 100,
      "curves": curves.get(idx), "source": "built-in-ecm-work-table",
      "elapsed_ms": _ecm_elapsed_ms(t0),
   }
}

fn _ecm_point(any x, any y) dict {
   {"inf": false, "x": _ecm_z(x), "y": _ecm_z(y)}
}

fn _ecm_inf() dict {
   {"inf": true}
}

fn _ecm_result(any point, any factor=nil) dict {
   {"point": point, "factor": factor}
}

fn _ecm_add(any p, any q, any a, any n) dict {
   def nn = _ecm_z(n)
   if p == nil || p.get("inf", false) { return _ecm_result(q) }
   if q == nil || q.get("inf", false) { return _ecm_result(p) }
   def x1, y1 = _ecm_z(p.get("x")), _ecm_z(p.get("y"))
   def x2, y2 = _ecm_z(q.get("x")), _ecm_z(q.get("y"))
   mut num = Z(0)
   mut den = Z(0)
   if x1 == x2 && mod(y1 + y2, nn) == Z(0) { return _ecm_result(_ecm_inf()) }
   if x1 == x2 && y1 == y2 {
      num = mod(Z(3) * x1 * x1 + _ecm_z(a), nn)
      den = mod(Z(2) * y1, nn)
   } else {
      num = mod(y2 - y1, nn)
      den = mod(x2 - x1, nn)
   }
   def g = gcd(den, nn)
   if _ecm_nontrivial(g, nn) { return _ecm_result(_ecm_inf(), g) }
   if g == nn { return _ecm_result(_ecm_inf()) }
   def inv = inverse_mod(den, nn)
   if inv == Z(0) { return _ecm_result(_ecm_inf()) }
   def lam = mod(num * inv, nn)
   def x3 = mod(lam * lam - x1 - x2, nn)
   def y3 = mod(lam * (x1 - x3) - y1, nn)
   _ecm_result(_ecm_point(x3, y3))
}

fn _ecm_mul(any p, any k, any a, any n) dict {
   mut acc = _ecm_inf()
   mut base = p
   mut kk = _ecm_z(k)
   while kk > Z(0) {
      if kk % Z(2) == Z(1) {
         def ar = _ecm_add(acc, base, a, n)
         if ar.get("factor", nil) != nil { return ar }
         acc = ar.get("point")
      }
      kk = kk / Z(2)
      if kk > Z(0) {
         def br = _ecm_add(base, base, a, n)
         if br.get("factor", nil) != nil { return br }
         base = br.get("point")
      }
   }
   _ecm_result(acc)
}

fn _ecm_prime_power(int p, int B1) bigint {
   mut q = Z(p)
   while q * Z(p) <= Z(B1) { q = q * Z(p) }
   q
}

fn _ecm_stage1_scalars(int B1) list {
   if _ecm_stage1_scalars_cache == nil { _ecm_stage1_scalars_cache = dict() }
   def key = to_str(B1)
   def cached = _ecm_stage1_scalars_cache.get(key, nil)
   if cached != nil { return cached }
   mut out = []
   mut prime = 2
   while prime <= B1 {
      out = out.append(_ecm_prime_power(prime, B1))
      prime = int(next_prime(prime))
   }
   _ecm_stage1_scalars_cache = _ecm_stage1_scalars_cache.set(key, out)
   out
}

fn _ecm_stage1_product_scalar(list scalars) bigint {
   mut k = Z(1)
   mut i = 0
   while i < scalars.len {
      k = k * _ecm_z(scalars.get(i))
      i += 1
   }
   k
}

fn _ecm_stage2_product_scalar(list scalars) bigint {
   _ecm_stage1_product_scalar(scalars)
}

fn _ecm_stage2_scalars(int B1, int B2) list {
   if _ecm_stage2_scalars_cache == nil { _ecm_stage2_scalars_cache = dict() }
   def key = to_str(B1) + ":" + to_str(B2)
   def cached = _ecm_stage2_scalars_cache.get(key, nil)
   if cached != nil { return cached }
   mut out = []
   if B2 <= B1 { return out }
   mut prime = int(next_prime(B1))
   while prime <= B2 {
      out = out.append(Z(prime))
      prime = int(next_prime(prime))
   }
   _ecm_stage2_scalars_cache = _ecm_stage2_scalars_cache.set(key, out)
   out
}

fn _ecm_stage1_product_for(int B1) bigint {
   if _ecm_stage1_product_cache == nil { _ecm_stage1_product_cache = dict() }
   def key = to_str(B1)
   def cached = _ecm_stage1_product_cache.get(key, nil)
   if cached != nil { return cached }
   def prod = _ecm_stage1_product_scalar(_ecm_stage1_scalars(B1))
   _ecm_stage1_product_cache = _ecm_stage1_product_cache.set(key, prod)
   prod
}

fn _ecm_stage2_product_for(int B1, int B2) bigint {
   if _ecm_stage2_product_cache == nil { _ecm_stage2_product_cache = dict() }
   def key = to_str(B1) + ":" + to_str(B2)
   def cached = _ecm_stage2_product_cache.get(key, nil)
   if cached != nil { return cached }
   def prod = _ecm_stage2_product_scalar(_ecm_stage2_scalars(B1, B2))
   _ecm_stage2_product_cache = _ecm_stage2_product_cache.set(key, prod)
   prod
}

fn _ecm_curve_report(any n, int B1, int curve, int B2=0) dict {
   def nn = _ecm_abs(n)
   def a = Z(curve + 1)
   def x = Z(2 + curve)
   def y = Z(3 + curve * 2)
   def b = mod(y * y - x * x * x - a * x, nn)
   def disc = mod(Z(4) * a * a * a + Z(27) * b * b, nn)
   mut out = {"curve": curve, "a": a, "b": b, "x": x, "y": y, "B1": B1, "B2": B2, "success": false}
   def gd = gcd(disc, nn)
   if _ecm_nontrivial(gd, nn) {
      return out.merge({"factor": gd, "success": true, "status": "singular-curve"})
   }
   mut pnt = _ecm_point(x, y)
   mut prime = 2
   mut stage1_ops = 0
   while prime <= B1 {
      def q = _ecm_prime_power(prime, B1)
      def mr = _ecm_mul(pnt, q, a, nn)
      stage1_ops += 1
      def f = mr.get("factor", nil)
      if _ecm_nontrivial(f, nn) {
         return out.merge({
               "factor": f, "success": true, "stage": 1,
               "stage1_ops": stage1_ops, "stage2_ops": 0,
               "ops": stage1_ops, "status": "stage1-factor",
            })
      }
      pnt = mr.get("point")
      prime = int(next_prime(prime))
   }
   mut stage2_ops = 0
   if B2 > B1 {
      prime = int(next_prime(B1))
      while prime <= B2 {
         def mr2 = _ecm_mul(pnt, prime, a, nn)
         stage2_ops += 1
         def f2 = mr2.get("factor", nil)
         if _ecm_nontrivial(f2, nn) {
            return out.merge({
                  "factor": f2, "success": true, "stage": 2,
                  "stage1_ops": stage1_ops, "stage2_ops": stage2_ops,
                  "ops": stage1_ops + stage2_ops,
                  "point_after_stage1": pnt, "status": "stage2-factor",
               })
         }
         pnt = mr2.get("point")
         prime = int(next_prime(prime))
      }
   }
   out.merge({
         "stage": 0, "stage1_ops": stage1_ops, "stage2_ops": stage2_ops,
         "point_after_stage1": pnt, "ops": stage1_ops + stage2_ops,
         "status": "not-found",
      })
}

fn _ecm_mont_point(any x, any z) list { [_ecm_z(x), _ecm_z(z)] }

fn _ecm_mont_x(list p) bigint { _ecm_z(p[0]) }

fn _ecm_mont_z(list p) bigint { _ecm_z(p[1]) }

fn _ecm_mont_setup(any n, int sigma) dict {
   def nn = _ecm_abs(n)
   def sz = Z(sigma)
   def u = mod(sz * sz - Z(5), nn)
   def v = mod(Z(4) * sz, nn)
   def x = mod(u * u * u, nn)
   def z = mod(v * v * v, nn)
   def den = mod(Z(4) * x * v, nn)
   def gd = gcd(den, nn)
   mut out = {"sigma": sigma, "u": u, "v": v, "x": x, "z": z, "success": false}
   if _ecm_nontrivial(gd, nn) {
      return out.merge({"factor": gd, "status": "setup-factor"})
   }
   if gd != Z(1) { return out.merge({"status": "singular-or-trivial-setup"}) }
   def inv_den = inverse_mod(den, nn)
   def inv4 = inverse_mod(Z(4), nn)
   if inv_den == Z(0) || inv4 == Z(0) { return out.merge({"status": "setup-inverse-failed"}) }
   def vu = mod(v - u, nn)
   def num = mod(vu * vu * vu * (Z(3) * u + v), nn)
   def A = mod(num * inv_den - Z(2), nn)
   def A24 = mod((A + Z(2)) * inv4, nn)
   out.merge({"A": A, "A24": A24, "point": _ecm_mont_point(x, z), "success": true, "status": "ok"})
}

fn _ecm_mont_mul(any p, any k, any a24, any n) dict {
   def kk = _ecm_z(k)
   if kk <= Z(0) { return _ecm_mont_point(Z(1), Z(0)) }
   if kk == Z(1) { return p }
   def nn = _ecm_abs(n)
   def px = _ecm_mont_x(p)
   def pz = _ecm_mont_z(p)
   def a24z = _ecm_z(a24)
   mut r0x = px
   mut r0z = pz
   mut t1 = mod(px + pz, nn)
   mut t2 = mod(px - pz, nn)
   mut aa = mod(t1 * t1, nn)
   mut bb = mod(t2 * t2, nn)
   mut e = mod(aa - bb, nn)
   mut r1x = mod(aa * bb, nn)
   mut r1z = mod(e * mod(bb + a24z * e, nn), nn)
   mut bit = bit_length(kk) - 2
   while bit >= 0 {
      def is_one = (kk & (Z(1) << bit)) != Z(0)
      if is_one {
         def ap = mod(r0x + r0z, nn)
         def bp = mod(r0x - r0z, nn)
         def cp = mod(r1x + r1z, nn)
         def dp = mod(r1x - r1z, nn)
         def da = mod(dp * ap, nn)
         def cb = mod(cp * bp, nn)
         def nr0x = mod(pz * mod((da + cb) * (da + cb), nn), nn)
         def nr0z = mod(px * mod((da - cb) * (da - cb), nn), nn)
         t1 = mod(r1x + r1z, nn)
         t2 = mod(r1x - r1z, nn)
         aa = mod(t1 * t1, nn)
         bb = mod(t2 * t2, nn)
         e = mod(aa - bb, nn)
         r1x = mod(aa * bb, nn)
         r1z = mod(e * mod(bb + a24z * e, nn), nn)
         r0x = nr0x
         r0z = nr0z
      } else {
         def ap = mod(r0x + r0z, nn)
         def bp = mod(r0x - r0z, nn)
         def cp = mod(r1x + r1z, nn)
         def dp = mod(r1x - r1z, nn)
         def da = mod(dp * ap, nn)
         def cb = mod(cp * bp, nn)
         def nr1x = mod(pz * mod((da + cb) * (da + cb), nn), nn)
         def nr1z = mod(px * mod((da - cb) * (da - cb), nn), nn)
         t1 = mod(r0x + r0z, nn)
         t2 = mod(r0x - r0z, nn)
         aa = mod(t1 * t1, nn)
         bb = mod(t2 * t2, nn)
         e = mod(aa - bb, nn)
         r0x = mod(aa * bb, nn)
         r0z = mod(e * mod(bb + a24z * e, nn), nn)
         r1x = nr1x
         r1z = nr1z
      }
      bit -= 1
   }
   _ecm_mont_point(r0x, r0z)
}

fn _ecm_mont_batch_factor(any z_product, list z_values, any n) dict {
   def nn = _ecm_abs(n)
   mut checks = 1
   def g = gcd(_ecm_z(z_product), nn)
   mut out = {"gcd_checks": checks, "factor": nil, "ambiguous": false}
   if _ecm_nontrivial(g, nn) { return out.set("factor", g) }
   if g == nn {
      out = out.set("ambiguous", true)
      mut i = 0
      while i < z_values.len {
         checks += 1
         def gi = gcd(_ecm_z(z_values.get(i)), nn)
         if _ecm_nontrivial(gi, nn) {
            out = out.set("gcd_checks", checks)
            return out.set("factor", gi)
         }
         i += 1
      }
      out = out.set("gcd_checks", checks)
   }
   out
}

fn _ecm_mont_curve_status(dict out, any factor, bool success, int stage, int stage1_ops, int stage2_ops, int scalar_bits, int gcd_checks, int ambiguous_batches, any pnt, str status) dict {
   mut fields = {
      "success": success, "stage": stage, "stage1_ops": stage1_ops,
      "stage2_ops": stage2_ops, "scalar_bits": scalar_bits,
      "gcd_checks": gcd_checks, "ambiguous_batches": ambiguous_batches,
      "point_after_stage1": pnt, "status": status,
   }
   if factor != nil { fields = fields.set("factor", factor) }
   out.merge(fields)
}

fn _ecm_mont_curve_report(any n, int B1, int curve, int B2=0, int sigma_start=6, int gcd_interval=16, any stage1_scalars=nil, any stage2_scalars=nil, any stage1_product_in=nil, any stage2_product_in=nil) dict {
   def nn = _ecm_abs(n)
   def sigma = sigma_start + curve
   def setup = _ecm_mont_setup(nn, sigma)
   def interval = max(1, gcd_interval)
   def s1 = stage1_scalars == nil ? _ecm_stage1_scalars(B1) : stage1_scalars
   def s2 = stage2_scalars == nil ? _ecm_stage2_scalars(B1, B2) : stage2_scalars
   mut out = {
      "curve": curve, "curve_model": "montgomery", "sigma": sigma,
      "B1": B1, "B2": B2, "batch_gcd_interval": interval,
      "stage1_scalar_count": s1.len, "stage2_scalar_count": s2.len,
      "setup": setup, "success": false,
   }
   def setup_factor = setup.get("factor", nil)
   if _ecm_nontrivial(setup_factor, nn) {
      return out.merge({"factor": setup_factor, "success": true, "stage": 0, "stage1_ops": 0, "stage2_ops": 0, "gcd_checks": 0, "status": "setup-factor"})
   }
   if !setup.get("success", false) { return out.set("status", setup.get("status", "setup-failed")) }
   def a24 = setup.get("A24")
   mut pnt = setup.get("point")
   mut stage1_ops = 0
   mut scalar_bits = 0
   mut gcd_checks = 0
   mut ambiguous_batches = 0
   mut pending_z = Z(1)
   mut pending_zs = []
   def stage1_product = stage1_product_in == nil ? _ecm_stage1_product_scalar(s1) : stage1_product_in
   out = out.merge({
         "stage1_kernel": "prime-power-product-ladder",
         "stage1_product_bits": bit_length(stage1_product),
      })
   if stage1_product > Z(1) {
      pnt = _ecm_mont_mul(pnt, stage1_product, a24, nn)
      stage1_ops = 1
      scalar_bits += bit_length(stage1_product)
      def z = _ecm_mont_z(pnt)
      def br = _ecm_mont_batch_factor(z, [z], nn)
      gcd_checks += int(br.get("gcd_checks", 0))
      if br.get("ambiguous", false) { ambiguous_batches += 1 }
      def f = br.get("factor", nil)
      if _ecm_nontrivial(f, nn) {
         return _ecm_mont_curve_status(out, f, true, 1, stage1_ops, 0, scalar_bits, gcd_checks, ambiguous_batches, pnt, "montgomery-stage1-factor")
      }
      if br.get("ambiguous", false) {
         pnt = setup.get("point")
         pending_z = Z(1)
         pending_zs = []
         stage1_ops = 0
         scalar_bits = 0
         mut si_fallback = 0
         while si_fallback < s1.len {
            def q = _ecm_z(s1.get(si_fallback))
            pnt = _ecm_mont_mul(pnt, q, a24, nn)
            stage1_ops += 1
            scalar_bits += bit_length(q)
            def zf = _ecm_mont_z(pnt)
            pending_z = mod(pending_z * zf, nn)
            pending_zs = pending_zs.append(zf)
            if (stage1_ops % interval) == 0 {
               def brf = _ecm_mont_batch_factor(pending_z, pending_zs, nn)
               gcd_checks += int(brf.get("gcd_checks", 0))
               if brf.get("ambiguous", false) { ambiguous_batches += 1 }
               def ff = brf.get("factor", nil)
               if _ecm_nontrivial(ff, nn) {
                  out = out.set("stage1_kernel", "prime-power-product-ladder-with-ambiguous-fallback")
                  return _ecm_mont_curve_status(out, ff, true, 1, stage1_ops, 0, scalar_bits, gcd_checks, ambiguous_batches, pnt, "montgomery-stage1-factor")
               }
               pending_z = Z(1)
               pending_zs = []
            }
            si_fallback += 1
         }
         if pending_zs.len > 0 {
            def tail = _ecm_mont_batch_factor(pending_z, pending_zs, nn)
            gcd_checks += int(tail.get("gcd_checks", 0))
            if tail.get("ambiguous", false) { ambiguous_batches += 1 }
            def tf = tail.get("factor", nil)
            if _ecm_nontrivial(tf, nn) {
               out = out.set("stage1_kernel", "prime-power-product-ladder-with-ambiguous-fallback")
               return _ecm_mont_curve_status(out, tf, true, 1, stage1_ops, 0, scalar_bits, gcd_checks, ambiguous_batches, pnt, "montgomery-stage1-factor")
            }
         }
      }
   }
   pending_z = Z(1)
   pending_zs = []
   mut stage2_ops = 0
   if B2 > B1 {
      def stage2_start = pnt
      def stage2_scalar_bits_before = scalar_bits
      def stage2_product = stage2_product_in == nil ? _ecm_stage2_product_scalar(s2) : stage2_product_in
      out = out.merge({
            "stage2_kernel": "prime-product-ladder",
            "stage2_product_bits": bit_length(stage2_product),
         })
      if stage2_product > Z(1) {
         pnt = _ecm_mont_mul(pnt, stage2_product, a24, nn)
         stage2_ops = 1
         scalar_bits += bit_length(stage2_product)
         def z2 = _ecm_mont_z(pnt)
         def br2 = _ecm_mont_batch_factor(z2, [z2], nn)
         gcd_checks += int(br2.get("gcd_checks", 0))
         if br2.get("ambiguous", false) { ambiguous_batches += 1 }
         def f2 = br2.get("factor", nil)
         if _ecm_nontrivial(f2, nn) {
            return _ecm_mont_curve_status(out, f2, true, 2, stage1_ops, stage2_ops, scalar_bits, gcd_checks, ambiguous_batches, pnt, "montgomery-stage2-factor")
         }
         if br2.get("ambiguous", false) {
            pnt = stage2_start
            pending_z = Z(1)
            pending_zs = []
            stage2_ops = 0
            scalar_bits = stage2_scalar_bits_before
            mut si = 0
            while si < s2.len {
               def sp = _ecm_z(s2.get(si))
               pnt = _ecm_mont_mul(pnt, sp, a24, nn)
               stage2_ops += 1
               scalar_bits += bit_length(sp)
               def zf2 = _ecm_mont_z(pnt)
               pending_z = mod(pending_z * zf2, nn)
               pending_zs = pending_zs.append(zf2)
               if (stage2_ops % interval) == 0 {
                  def brf2 = _ecm_mont_batch_factor(pending_z, pending_zs, nn)
                  gcd_checks += int(brf2.get("gcd_checks", 0))
                  if brf2.get("ambiguous", false) { ambiguous_batches += 1 }
                  def ff2 = brf2.get("factor", nil)
                  if _ecm_nontrivial(ff2, nn) {
                     out = out.set("stage2_kernel", "prime-product-ladder-with-ambiguous-fallback")
                     return _ecm_mont_curve_status(out, ff2, true, 2, stage1_ops, stage2_ops, scalar_bits, gcd_checks, ambiguous_batches, pnt, "montgomery-stage2-factor")
                  }
                  pending_z = Z(1)
                  pending_zs = []
               }
               si += 1
            }
         }
      }
   }
   if pending_zs.len > 0 {
      def tail2 = _ecm_mont_batch_factor(pending_z, pending_zs, nn)
      gcd_checks += int(tail2.get("gcd_checks", 0))
      if tail2.get("ambiguous", false) { ambiguous_batches += 1 }
      def tf2 = tail2.get("factor", nil)
      if _ecm_nontrivial(tf2, nn) {
         return _ecm_mont_curve_status(out, tf2, true, 2, stage1_ops, stage2_ops, scalar_bits, gcd_checks, ambiguous_batches, pnt, "montgomery-stage2-factor")
      }
   }
   _ecm_mont_curve_status(out, nil, false, 0, stage1_ops, stage2_ops, scalar_bits, gcd_checks, ambiguous_batches, pnt, "not-found")
}

fn montgomery_ecm_factor_report(any n, int B1=1000, int curves=32, int B2=0, int sigma_start=6, int gcd_interval=16) dict {
   "Run deterministic Montgomery x-only ECM with Suyama-style sigma curves."
   def t0 = ticks()
   def nn = _ecm_abs(n)
   mut attempts = []
   mut out = {
      "method": B2 > B1 ? "montgomery-ecm-stage1-stage2" : "montgomery-ecm-stage1",
      "curve_model": "montgomery", "n_bits": bit_length(nn), "B1": B1, "B2": B2,
      "curves": curves, "sigma_start": sigma_start,
      "batch_gcd_interval": max(1, gcd_interval), "success": false,
   }
   if nn <= Z(3) { return out.set("status", "invalid-or-prime") }
   if nn % Z(2) == Z(0) { return out.merge({"factor": Z(2), "success": true, "status": "even"}) }
   mut c, total_gcd_checks, total_stage1_ops, total_stage2_ops = 0, 0, 0, 0
   def s1 = _ecm_stage1_scalars(B1)
   def s2 = _ecm_stage2_scalars(B1, B2)
   def s1_product = _ecm_stage1_product_for(B1)
   def s2_product = B2 > B1 ? _ecm_stage2_product_for(B1, B2) : Z(1)
   out = out.merge({"stage1_scalar_count": s1.len, "stage2_scalar_count": s2.len})
   while c < curves {
      def cr = _ecm_mont_curve_report(nn, B1, c, B2, sigma_start, gcd_interval, s1, s2, s1_product, s2_product)
      attempts = attempts.append(cr)
      total_gcd_checks += int(cr.get("gcd_checks", 0))
      total_stage1_ops += int(cr.get("stage1_ops", 0))
      total_stage2_ops += int(cr.get("stage2_ops", 0))
      if cr.get("success", false) {
         return out.merge({
               "factor": cr.get("factor"), "success": true, "curve": c,
               "sigma": cr.get("sigma", sigma_start + c), "stage": cr.get("stage", 0),
               "attempts": attempts, "total_gcd_checks": total_gcd_checks,
               "total_stage1_ops": total_stage1_ops, "total_stage2_ops": total_stage2_ops,
               "elapsed_ms": _ecm_elapsed_ms(t0), "status": cr.get("status", "factor"),
            })
      }
      c += 1
   }
   out.merge({
         "attempts": attempts, "total_gcd_checks": total_gcd_checks,
         "total_stage1_ops": total_stage1_ops, "total_stage2_ops": total_stage2_ops,
         "elapsed_ms": _ecm_elapsed_ms(t0), "status": "not-found",
      })
}

fn montgomery_ecm_factor(any n, int B1=1000, int curves=32, int B2=0, int sigma_start=6, int gcd_interval=16) any {
   "Return one non-trivial factor found by Montgomery ECM, or nil."
   montgomery_ecm_factor_report(n, B1, curves, B2, sigma_start, gcd_interval).get("factor", nil)
}

fn ecm_scheduled_factor_report(any n, bool deep=false, int max_curves=24, int max_B1=2000, int max_B2=5000, int batch_size=8) dict {
   "Run bounded batched Montgomery ECM using the built-in work plan."
   def t0 = ticks()
   def nn = _ecm_abs(n)
   def plan = ecm_work_plan_report(nn, deep)
   def B1 = min(int(plan.get("B1", max_B1)), max_B1)
   def B2 = min(int(plan.get("B2", max_B2)), max_B2)
   def curves = min(int(plan.get("curves", max_curves)), max_curves)
   def bs = max(1, batch_size)
   mut blocks = []
   mut curves_done, sigma, total_gcd_checks, total_stage1_ops, total_stage2_ops = 0, 6, 0, 0, 0
   mut out = {
      "method": "ecm-batched-schedule", "n_bits": bit_length(nn), "plan": plan,
      "B1": B1, "B2": B2, "curves": curves, "batch_size": bs, "success": false,
   }
   if nn <= Z(3) { return out.set("status", "invalid-or-prime") }
   if nn % Z(2) == Z(0) { return out.merge({"factor": Z(2), "success": true, "status": "even"}) }
   def pre_pm1_bound = int(plan.get("pre_pm1_bound", 0))
   out = out.merge({"pre_pm1_bound": pre_pm1_bound, "pre_pm1_attempted": false})
   if pre_pm1_bound > 1 {
      def pm1 = pollard.pollard_pm1_report(nn, pre_pm1_bound)
      out = out.merge({"pre_pm1_attempted": true, "pre_pm1_report": pm1})
      def pf = pm1.get("factor", nil)
      if _ecm_nontrivial(pf, nn) {
         return out.merge({
               "factor": pf, "success": true, "stage": -1, "blocks": blocks,
               "curves_done": 0, "total_gcd_checks": 0,
               "total_stage1_ops": int(pm1.get("stage1_ops", 0)),
               "total_stage2_ops": 0, "elapsed_ms": _ecm_elapsed_ms(t0),
               "status": "pm1-precheck-factor",
            })
      }
   }
   while curves_done < curves {
      def take = min(bs, curves - curves_done)
      def br = montgomery_ecm_factor_report(nn, B1, take, B2, sigma)
      blocks = blocks.append(br)
      total_gcd_checks += int(br.get("total_gcd_checks", 0))
      total_stage1_ops += int(br.get("total_stage1_ops", 0))
      total_stage2_ops += int(br.get("total_stage2_ops", 0))
      curves_done += take
      sigma += take
      if br.get("success", false) {
         return out.merge({
               "factor": br.get("factor"), "success": true, "stage": br.get("stage", 0),
               "winning_block": blocks.len - 1, "blocks": blocks, "curves_done": curves_done,
               "total_gcd_checks": total_gcd_checks, "total_stage1_ops": total_stage1_ops,
               "total_stage2_ops": total_stage2_ops, "elapsed_ms": _ecm_elapsed_ms(t0),
               "status": br.get("status", "factor"),
            })
      }
   }
   out.merge({
         "blocks": blocks, "curves_done": curves_done,
         "total_gcd_checks": total_gcd_checks, "total_stage1_ops": total_stage1_ops,
         "total_stage2_ops": total_stage2_ops, "elapsed_ms": _ecm_elapsed_ms(t0),
         "status": "not-found",
      })
}

fn _ecm_batch_lane_entry(any raw, int index, bool arbitrary_precheck, int max_curves, int max_B1, int max_B2) dict {
   def nn = _ecm_abs(raw)
   def plan = micro_ecm_work_plan_report(nn, arbitrary_precheck)
   def bits = int(plan.get("n_bits", 0))
   mut entry = {
      "index": index, "n": nn, "n_bits": bits,
      "B1": min(int(plan.get("B1", max_B1)), max_B1),
      "B2": min(int(plan.get("B2", max_B2)), max_B2),
      "curves": min(int(plan.get("curves", max_curves)), max_curves),
      "pre_pm1_bound": int(plan.get("pre_pm1_bound", 0)),
      "success": false,
   }
   if nn <= Z(3) { return entry.set("status", "invalid-or-prime") }
   if nn % Z(2) == Z(0) { return entry.merge({"factor": Z(2), "success": true, "status": "even"}) }
   if arbitrary_precheck {
      mut pi = 0
      def pre = plan.get("pre_attempts", [])
      while pi < pre.len {
         def p = pre.get(pi)
         def pr = montgomery_ecm_factor_report(nn, int(p.get("B1", 47)), int(p.get("curves", 1)), int(p.get("B2", 47 * 25)), 6 + pi)
         def pf = pr.get("factor", nil)
         if _ecm_nontrivial(pf, nn) {
            return entry.merge({
                  "factor": pf, "success": true, "stage": pr.get("stage", 0),
                  "pre_attempt": pi, "pre_report": pr, "status": "pre-ecm-factor",
               })
         }
         pi += 1
      }
   }
   def pm1_bound = int(entry.get("pre_pm1_bound", 0))
   if pm1_bound > 1 {
      def pm1 = pollard.pollard_pm1_report(nn, pm1_bound)
      def pf = pm1.get("factor", nil)
      if _ecm_nontrivial(pf, nn) {
         return entry.merge({
               "factor": pf, "success": true, "stage": -1,
               "pre_pm1_report": pm1, "status": "pm1-precheck-factor",
            })
      }
      entry = entry.set("pre_pm1_report", pm1)
   }
   def rep = montgomery_ecm_factor_report(nn, int(entry.get("B1")), int(entry.get("curves")), int(entry.get("B2")), 6)
   entry.merge({
         "factor": rep.get("factor", nil), "success": rep.get("success", false),
         "stage": rep.get("stage", 0), "report": rep,
         "status": rep.get("status", "not-found"),
      })
}

fn ecm_batch_lane_factor_report(list values, bool arbitrary_precheck=false, int lane_width=8, int max_curves=42, int max_B1=205, int max_B2=5125) dict {
   "Run the small-cofactor ECM list dispatcher over values and return per-lane reports."
   def t0 = ticks()
   def width = max(1, lane_width)
   mut reports = []
   mut blocks = []
   mut i = 0
   mut found = 0
   while i < values.len {
      def end = min(values.len, i + width)
      mut block = {"start": i, "end": end, "lane_width": width, "reports": []}
      mut j = i
      while j < end {
         def r = _ecm_batch_lane_entry(values.get(j), j, arbitrary_precheck, max_curves, max_B1, max_B2)
         if r.get("success", false) { found += 1 }
         reports = reports.append(r)
         block = block.set("reports", block.get("reports").append(r))
         j += 1
      }
      blocks = blocks.append(block)
      i = end
   }
   {
      "method": "ecm-batch-lane-dispatch",
      "source_model": "batched uECM dispatch",
      "lane_width": width, "inputs": values.len, "blocks": blocks,
      "reports": reports, "factors_found": found,
      "all_done": reports.len == values.len,
      "elapsed_ms": _ecm_elapsed_ms(t0),
   }
}

fn ecm_batch_lane_factor(list values, bool arbitrary_precheck=false, int lane_width=8) list {
   "Return factors from ecm_batch_lane_factor_report in input order."
   def rep = ecm_batch_lane_factor_report(values, arbitrary_precheck, lane_width)
   mut out = []
   mut i = 0
   def rows = rep.get("reports", [])
   while i < rows.len {
      out = out.append(rows.get(i).get("factor", nil))
      i += 1
   }
   out
}

fn ecm_factor_report(any n, int B1=1000, int curves=32, int B2=0) dict {
   "Run deterministic ECM and return per-curve stage diagnostics."
   def t0 = ticks()
   def nn = _ecm_abs(n)
   def mont = montgomery_ecm_factor_report(nn, B1, curves, B2)
   if mont.get("success", false) { return mont.set("elapsed_ms", _ecm_elapsed_ms(t0)) }
   mut attempts = []
   mut out = {
      "method": B2 > B1 ? "ecm-stage1-stage2" : "ecm-stage1",
      "preferred_report": mont, "curve_model": "affine-weierstrass-fallback",
      "n_bits": bit_length(nn), "B1": B1, "B2": B2, "curves": curves, "success": false,
   }
   if nn <= Z(3) { return out.set("status", "invalid-or-prime") }
   if nn % Z(2) == Z(0) { return out.merge({"factor": Z(2), "success": true, "status": "even"}) }
   mut c = 0
   while c < curves {
      def cr = _ecm_curve_report(nn, B1, c, B2)
      attempts = attempts.append(cr)
      if cr.get("success", false) {
         return out.merge({
               "factor": cr.get("factor"), "success": true, "curve": c,
               "stage": cr.get("stage", 0), "attempts": attempts,
               "elapsed_ms": _ecm_elapsed_ms(t0), "status": cr.get("status", "factor"),
            })
      }
      c += 1
   }
   out.merge({"attempts": attempts, "elapsed_ms": _ecm_elapsed_ms(t0), "status": "not-found"})
}

fn ecm_factor(any n, int B1=1000, int curves=32, int B2=0) any {
   "Return one non-trivial factor found by ECM, or nil."
   ecm_factor_report(n, B1, curves, B2).get("factor", nil)
}

#main {
   assert(ecm_factor(Z(10403), 1000, 16, 5000) != nil, "ecm factor")
   assert(ecm_factor(Z(7), 100, 1) == nil, "ecm no factor for prime")
   assert(montgomery_ecm_factor(Z(10403), 1000, 16, 5000) != nil, "mont ecm factor")
   assert(ecm_scheduled_factor_report(Z(10403), false, 24, 2000, 5000) != nil, "ecm schedule")
   assert(ecm_work_plan_report(Z(100)) != nil, "ecm work plan")
   def n_prime = Z(1000000007)
   def rep = montgomery_ecm_factor_report(n_prime, 205, 4, 0, 6, 16)
   assert(!rep.get("success", false), "prime ECM does not factor")
   assert(rep.get("status", "") == "not-found", "prime ECM status")
   assert(rep.get("attempts").get(0).get("stage1_kernel", "") == "prime-power-product-ladder", "product scalar stage1 kernel")
   assert(int(rep.get("total_stage1_ops", 0)) == 4, "one product ladder per curve")
   def rep2 = montgomery_ecm_factor_report(n_prime, 70, 2, 350, 6, 16)
   assert(!rep2.get("success", false), "stage2 prime ECM does not factor")
   assert(rep2.get("attempts").get(0).get("stage2_kernel", "") == "prime-product-ladder", "product scalar stage2 kernel")
   assert(int(rep2.get("total_stage2_ops", 0)) == 2, "one stage2 product ladder per curve")
   print("✓ std.math.crypto.factorization.ecm self-test passed")
}
