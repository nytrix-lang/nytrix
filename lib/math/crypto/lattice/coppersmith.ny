;; Keywords: lattice coppersmith math crypto number-theory
;; Lattice routines for Coppersmith small-root lattice construction.
;; Reference:
;; - https://www.cs.cmu.edu/~afs/cs/project/quake/public/papers/Coppersmith-Crypto96.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/lowRSAexp.pdf
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.coppersmith(howgrave_graham, coppersmith_univariate, coppersmith_univariate_report, coppersmith_univariate_plan, poly_pow, coppersmith_bivariate, coppersmith_coron_integer_bivariate, coppersmith_herrmann_may_bivariate, coppersmith_multivariate_heuristic)
use std.core
use std.math.scalar (ceil, floor, float)
use std.math.nt
use std.math.crypto.poly
use std.math.crypto.lattice.lll
use std.math.crypto.lattice.small_roots
use std.math.crypto.lattice.mvpoly

fn _poly_copy_local(list p) list {
   mut out = []
   mut i = 0
   while i < p.len {
      out = out.append(p.get(i))
      i += 1
   }
   out
}

fn poly_pow(list p, int e) list {
   "Compute polynomial p raised to integer power e via binary exponentiation.
   p is a list of coefficients in ascending order ; e is non-negative."
   def early = e == 0 ? [1] : ((e == 1) ? _poly_copy_local(p) : nil)
   if early != nil { return early }
   mut res = [1]
   mut base = _poly_copy_local(p)
   mut exp = e
   while exp > 0 {
      res = (exp % 2 == 1) ? poly_mul(res, base) : res
      base = poly_mul(base, base)
      exp = exp / 2
   }
   res
}

fn poly_shift(list p, int s) list {
   "Multiply polynomial p by x^s.
   Shifts coefficients by s positions with leading zero padding."
   mut result = list(0)
   mut i = 0
   while i < s {
      result = result.append(0)
      i += 1
   }
   i = 0
   def n = p.len
   while i < n {
      result = result.append(p.get(i))
      i += 1
   }
   result
}

fn howgrave_graham(any f, any N, any m, any t, any X, str reduction_method="ny") list {
   "Howgrave-Graham small-roots algorithm for f(x) = 0 mod N^m.
   f is monic ; N is modulus; m/t are shift controls; X is root bound."
   modular_univariate(f, N, m, t, X, nil, reduction_method)
}

fn _ceil_int_pos(any x) int {
   def v = int(ceil(float(x)))
   v < 1 ? 1 : v
}

fn _floor_int_nonnegative(any x) int {
   def v = int(floor(float(x)))
   v < 0 ? 0 : v
}

fn _coppersmith_auto_X(any N, int degree, any beta, any epsilon) bigint {
   def exponent = float(beta) * float(beta) / float(max(1, degree)) - float(epsilon)
   if exponent <= 0.0 { return Z(1) }
   def bits = float(bit_length(N)) * exponent - 1.0
   def shift = _ceil_int_pos(bits)
   bigint_lshift(Z(1), shift)
}

fn _coppersmith_min_factor_bound(any N, any beta) bigint {
   if float(beta) >= 0.999999 { return Z(N) }
   def bits = int(floor(float(bit_length(N)) * float(beta))) - 2
   if bits <= 0 { return Z(1) }
   bigint_lshift(Z(1), bits)
}

fn coppersmith_univariate_plan(any f, any N, any X=nil, any beta=1.0, any m=nil, any t=nil, any epsilon=nil, str reduction_method="ny") dict {
   "Return the derived parameter plan for univariate Coppersmith.
   This mirrors Sage-style inspectability: callers can see degree, beta,
   epsilon, m/t, bound X, reduction method, and the validation factor bound before reduction."
   mut out = dict(16)
   out = out.set("valid", false)
   out = out.set("reason", "ok")
   if !is_list(f) || f.len <= 1 {
      out = out.set("reason", "polynomial must be a coefficient list with degree >= 1")
      return out
   }
   def d = f.len - 1
   def beta_f = float(beta)
   if beta_f <= 0.0 || beta_f > 1.0 {
      out = out.set("reason", "beta must satisfy 0 < beta <= 1")
      return out
   }
   def eps_f = epsilon == nil ? beta_f / 8.0 : float(epsilon)
   if eps_f <= 0.0 {
      out = out.set("reason", "epsilon must be positive")
      return out
   }
   def may_m = _ceil_int_pos((beta_f * beta_f) / (float(d) * eps_f))
   def safety_m = _ceil_int_pos((7.0 * beta_f) / float(d))
   def mm = m == nil ? max(may_m, safety_m) : int(m)
   def tt = t == nil ? _floor_int_nonnegative(float(d) * float(mm) * (1.0 / beta_f - 1.0)) : int(t)
   def XX = X == nil ? _coppersmith_auto_X(N, d, beta_f, eps_f) : X
   out = out.set("valid", true)
   out = out.set("degree", d)
   out = out.set("n_bits", bit_length(Z(N)))
   out = out.set("beta", beta_f)
   out = out.set("epsilon", eps_f)
   out = out.set("m", mm)
   out = out.set("t", tt)
   out = out.set("X", XX)
   out = out.set("min_factor", _coppersmith_min_factor_bound(N, beta_f))
   out = out.set("auto_m", may_m)
   out = out.set("safety_m", safety_m)
   out = out.set("reduction_method", reduction_method)
   out
}

fn coppersmith_univariate(any f, any N, any X=nil, any beta=1.0, any m=nil, any t=nil, any epsilon=nil, str reduction_method="ny") list {
   "Automated univariate Coppersmith small-roots solver.
   Uses May-style defaults:
   - epsilon defaults to beta/8
   - m = max(ceil(beta^2/(degree*epsilon)), ceil(7*beta/degree))
   - t = floor(degree*m*(1/beta - 1))
   Explicit m/t/X override these defaults. `reduction_method=\"auto\"`
   stays on the same reduction path."
   if !is_list(f) || f.len <= 1 { return [] }
   def beta_f = float(beta)
   if beta_f <= 0.0 || beta_f > 1.0 { panic("CoppersmithParameterError: beta must satisfy 0 < beta <= 1") }
   def eps_f = epsilon == nil ? beta_f / 8.0 : float(epsilon)
   if eps_f <= 0.0 { panic("CoppersmithParameterError: epsilon must be positive") }
   def plan = coppersmith_univariate_plan(f, N, X, beta_f, m, t, eps_f, reduction_method)
   modular_univariate(f, N, plan.get("m"), plan.get("t"), plan.get("X"), plan.get("min_factor"), reduction_method)
}

fn coppersmith_univariate_report(any f, any N, any X=nil, any beta=1.0, any m=nil, any t=nil, any epsilon=nil, str reduction_method="ny") dict {
   "Report-first automated univariate Coppersmith small-roots solver.
   Compact callers should use coppersmith_univariate ; audit/debug callers can
   inspect the plan, lattice dimensions, reduction profile, roots tried, and
   validation status here."
   def beta_f = float(beta)
   def eps_f = epsilon == nil ? beta_f / 8.0 : float(epsilon)
   def plan = coppersmith_univariate_plan(f, N, X, beta_f, m, t, eps_f, reduction_method)
   mut out = dict(16)
   out = out.set("plan", plan)
   out = out.set("reduction_method", reduction_method)
   if !plan.get("valid", false) {
      out = out.set("success", false)
      out = out.set("reason", plan.get("reason", "invalid plan"))
      out = out.set("roots", [])
      return out
   }
   def rep = modular_univariate_report(f, N, plan.get("m"), plan.get("t"), plan.get("X"), plan.get("min_factor"), reduction_method)
   out = out.set("success", rep.get("success", false))
   out = out.set("reason", rep.get("reason", "unknown"))
   out = out.set("roots", rep.get("roots", []))
   out = out.set("root_count", rep.get("root_count", 0))
   out = out.set("lattice_rows", rep.get("lattice_rows", 0))
   out = out.set("lattice_cols", rep.get("lattice_cols", 0))
   out = out.set("reduction", rep.get("reduction", nil))
   out = out.set("profile_improvement_first_norm", rep.get("profile_improvement_first_norm", Z(0)))
   out = out.set("roots_tried", rep.get("common_roots_tried", 0) + rep.get("relation_polynomials_tried", 0))
   out = out.set("validation_status", rep.get("validation_status", "unknown"))
   out
}

fn coppersmith_bivariate(any f, any N, any X, any Y, any beta=0.5, any m=nil) list {
   "Bivariate Coppersmith method(Coron's approach) for polynomials f(x, y) = 0 mod N.
   Finds roots(x0, y0) such that |x0| < X and |y0| < Y."
   if !mv_is_poly(f) { return [] }
   mut mm = m == nil ? 2 : int(m)
   if mm < 1 { mm = 1 }
   mut tt = mm / 2
   if tt < 1 { tt = 1 }
   herrmann_may_modular_bivariate(f, N, mm, tt, X, Y)
}

fn coppersmith_herrmann_may_bivariate(any f, any N, any X, any Y, any m=2, any t=1) list {
   "Explicit Herrmann-May modular bivariate wrapper."
   if !mv_is_poly(f) { return [] }
   herrmann_may_modular_bivariate(f, N, int(m), int(t), X, Y)
}

fn coppersmith_coron_integer_bivariate(any f, any X, any Y, int k=2) list {
   "Coron direct integer bivariate wrapper."
   if !mv_is_poly(f) { return [] }
   coron_integer_bivariate(f, int(k), X, Y)
}

fn coppersmith_multivariate_heuristic(any f, any N, any bounds, any beta=1.0, any m=2, any t=1, str strategy="jochemsz_may") list {
   "Multivariate modular small-roots wrapper over Ny strategies.
   strategy:
   - `jochemsz_may` (default)
   - `herrmann_may`
   - `nitaj_fouotsa` (requires 3 bounds)"
   if !mv_is_poly(f) { return [] }
   mut mm = int(m)
   mut tt = int(t)
   if mm < 1 { mm = 1 }
   if tt < 0 { tt = 0 }
   if strategy == "herrmann_may" { return herrmann_may_modular_multivariate(f, N, mm, tt, bounds) }
   if strategy == "nitaj_fouotsa" {
      if !is_list(bounds) || bounds.len < 3 { return [] }
      def bx, by = bounds[0], bounds[1]
      def bz = bounds[2]
      return nitaj_fouotsa_modular_trivariate(f, N, mm, max(1, tt), bx, by, bz)
   }
   jochemsz_may_modular_multivariate(f, N, mm, bounds, "default")
}
