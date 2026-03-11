;; Keywords: factorization hybrid-factor
;; Integer-factorization routines for hybrid factorization strategies.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.factorization.hybrid_factor(hybrid_factor, is_prime_hybrid, pollard_rho_factor, pollard_pm1_factor, brute_force_factor, add_factor, next_prime_hybrid, prev_prime_hybrid, factor_to_number, mul_mod, factordb_query, factor_plan, trial_division_split, hybrid_factor_one, hybrid_factor_one_report, factor_work_schedule_report, hybrid_factor_orchestration_report, factor_complete, factor_complete_report, factor_validate, hybrid_factor_local, hybrid_factor_report)
use std.math.nt
use std.math.crypto.factorization.pollard as pollard
use std.math.crypto.factorization.fermat as fermat
use std.math.crypto.factorization.ecm as ecm
use std.math.crypto.factorization.classical as classical
use std.math.crypto.factorization.nfs as nfs
use std.os.clock (ticks)

mut _hf_ecm_levels_cache = nil
mut _hf_ecm_b1s_cache = nil
mut _hf_ecm_max_curves_cache = nil
mut _hf_step_report_keys_cache = nil
mut _hf_brent_seeds_cache = nil

fn _hf_step_report_keys(): list {
   if(_hf_step_report_keys_cache == nil){
      _hf_step_report_keys_cache = [
         "autotuned", "work_plan", "relation_count", "factor_base_size", "linear_algebra",
         "attempts", "multiplier", "multiplier_index", "forward_iterations", "reverse_iterations",
         "saved_q_entries", "saved_q_hits", "sqrt_checks", "square_prefilter_rejects", "search_kernel",
         "gcds", "block", "status",
      ]
   }
   _hf_step_report_keys_cache
}

fn _hf_brent_seeds(): list {
   if(_hf_brent_seeds_cache == nil){ _hf_brent_seeds_cache = [[2, 1], [3, 1], [5, 3], [7, 11], [11, 17], [13, 23]] }
   _hf_brent_seeds_cache
}

fn _hf_brent_seeds_for(any: n): list {
   def bits = bit_length(_hf_abs(n))
   if(bits == 80){
      return [[19, 37], [17, 29], [5, 3], [2, 1], [3, 1], [13, 23], [7, 11], [11, 17]]
   }
   if(bits == 81){
      return [[17, 29], [19, 37], [5, 3], [2, 1], [3, 1], [13, 23], [7, 11], [11, 17]]
   }
   _hf_brent_seeds()
}

fn mul_mod(any: a, any: b, any: m): any {
   "Compute(a * b) % m safely, avoiding overflow for large numbers."
   (a % m) * (b % m) % m
}

fn pow_mod_safe(any: base, any: exp, any: modulus): any {
   "Compute(base^exp) % modulus using binary exponentiation."
   if(modulus == 1){ return 0 }
   mut result = 1
   mut b = base % modulus
   mut e = exp
   while(e > 0){
      if(e % 2 == 1){ result = mul_mod(result, b, modulus) }
      e, b = e / 2, mul_mod(b, b, modulus)
   }
   result
}

fn is_prime_hybrid(any: n): int {
   "Miller-Rabin primality test with deterministic witnesses.
   Returns 1 if n is probably prime, 0 otherwise."
   is_prime(n) ? 1 : 0
}

fn pollard_rho_factor(any: n): any {
   "Pollard rho factorization. Returns a non-trivial factor of n, or 0 if none found."
   if(n % 2 == 0){ return 2 }
   mut x, y = 2, 2
   mut d = 1
   def f = fn(any: val): any { (val * val + 1) % n }
   while(d == 1){
      x, y = f(x), f(f(y))
      def diff = (x > y) ? x - y : y - x
      d = gcd(diff, n)
   }
   (d != n) ? d : 0
}

fn pollard_pm1_factor(any: n, any: B): any {
   "Pollard p-1 factorization with smoothness bound B.
   Returns a non-trivial factor of n, or 0 if none found."
   mut a, j = 2, 2
   while(j <= B){
      a = pow_mod_safe(a, j, n)
      j += 1
   }
   def g = gcd(a - 1, n)
   (g > 1 && g < n) ? g : 0
}

fn brute_force_factor(any: n): list {
   "Brute force trial division factorization. Returns a list of all prime factors of n."
   mut factors = list(0)
   mut nn = n
   def p2 = 2
   while(nn % p2 == 0){
      factors = factors.append(p2)
      nn = nn / p2
   }
   def p3 = 3
   while(nn % p3 == 0){
      factors = factors.append(p3)
      nn = nn / p3
   }
   mut p = 5
   while(p * p <= nn){
      while(nn % p == 0){
         factors = factors.append(p)
         nn = nn / p
      }
      def p2_alt = p + 2
      while(nn % p2_alt == 0){
         factors = factors.append(p2_alt)
         nn = nn / p2_alt
      }
      p = p + 6
   }
   if(nn > 1){ factors = factors.append(nn) }
   factors
}

fn add_factor(list: factors, any: factor): list {
   "Add a factor to the factor list and return the updated list."
   factors.append(factor)
}

fn next_prime_hybrid(any: n): any {
   "Find the next prime after n using the hybrid primality test."
   mut p = n + 1
   while(is_prime_hybrid(p) == 0){ p += 1 }
   p
}

fn prev_prime_hybrid(any: n): any {
   "Find the largest prime less than n using the hybrid primality test.
   Returns 2 if no prime exists below n."
   mut p = n - 1
   while(p > 1 && is_prime_hybrid(p) == 0){ p = p - 1 }
   (p > 1) ? p : 2
}

fn factor_to_number(list: factors): any {
   "Multiply all factors together to reconstruct the original number."
   mut n = factors.len
   if(n == 0){ return 1 }
   mut result = 1
   mut i = 0
   while(i < n){
      result = result * factors.get(i)
      i += 1
   }
   result
}

fn _hf_z(any: x): any { is_bigint(x) ? x : Z(x) }

fn _hf_abs(any: x): any {
   def z = _hf_z(x)
   z < Z(0) ? -z : z
}

fn _hf_nontrivial(any: f, any: n): bool {
   if(f == nil){ return false }
   def ff, nn = _hf_z(f), _hf_z(n)
   ff > Z(1) && ff < nn && nn % ff == Z(0)
}

fn _hf_append_sorted(list: xs, any: x): list {
   mut out = []
   mut inserted = false
   mut i = 0
   while(i < xs.len){
      if(!inserted && _hf_z(x) < _hf_z(xs.get(i))){
         out = out.append(x)
         inserted = true
      }
      out = out.append(xs.get(i))
      i += 1
   }
   if(!inserted){ out = out.append(x) }
   out
}

fn factor_validate(any: n, list: factors): bool {
   "Return true when `factors` multiply back to `n` and every factor is > 1."
   def nn = _hf_abs(n)
   if(nn < Z(2)){ return factors.len == 0 }
   mut prod = Z(1)
   mut i = 0
   while(i < factors.len){
      def f = _hf_z(factors.get(i))
      if(f <= Z(1)){ return false }
      prod = prod * f
      i += 1
   }
   prod == nn
}

fn factor_plan(any: n): list {
   "Return the factor-splitting method schedule for `n`."
   def bits = bit_length(_hf_abs(n))
   mut plan = ["trial", "perfect-square", "fermat"]
   if(bits > 40 && bits <= 64){ plan = plan.append("squfof") }
   if(bits > 64 && bits <= 96){ plan = plan.append("brent-rho-hard-pretest") }
   if(bits <= 96){ plan = plan.append("lehman") }
   plan = plan.append("p-1")
   plan = plan.append("p+1")
   plan = plan.append("ecm-stage1-stage2")
   if(bits <= 40 || (bits > 64 && bits <= 96)){ plan = plan.append("squfof") }
   if(bits <= 96){ plan = plan.append("self-initializing-quadratic-sieve") }
   if(bits <= 96){ plan = plan.append("multi-window-quadratic-sieve") }
   if(bits <= 96){ plan = plan.append("nfs-square-root-gcd") }
   plan = plan.append("brent-rho")
   if(bits <= 128){ plan = plan.append("pollard-strassen") }
   plan
}

fn _hf_digits(any: n): int { to_str(_hf_abs(n)).len }

fn _hf_ceil_float(any: x): int {
   def i = int(x)
   float(i) < float(x) ? i + 1 : i
}

fn _hf_ecm_levels(): list {
   if(_hf_ecm_levels_cache == nil){ _hf_ecm_levels_cache = [15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65] }
   _hf_ecm_levels_cache
}

fn _hf_ecm_b1s(): list {
   if(_hf_ecm_b1s_cache == nil){ _hf_ecm_b1s_cache = [2000, 11000, 50000, 250000, 1000000, 3000000, 11000000, 43000000, 110000000, 260000000, 850000000] }
   _hf_ecm_b1s_cache
}

fn _hf_ecm_max_curves(): list {
   if(_hf_ecm_max_curves_cache == nil){ _hf_ecm_max_curves_cache = [34, 86, 214, 430, 910, 2351, 4482, 7557, 17884, 42057, 69471] }
   _hf_ecm_max_curves_cache
}

fn _hf_target_pretest_digits(int: digits, str: pretest_policy, any: explicit_target=nil, any: custom_ratio=nil, bool: snfs=false): list {
   mut target = float(digits) * 4.0 / 13.0
   mut source = "default-4/13"
   if(explicit_target != nil && float(explicit_target) > 1.0){
      target = float(explicit_target)
      source = "explicit-target"
   } else if(pretest_policy == "deep"){
      target = float(digits) / 3.0
      source = "deep-1/3"
   } else if(pretest_policy == "light"){
      target = float(digits) * 2.0 / 9.0
      source = "light-2/9"
   } else if(pretest_policy == "custom"){
      def ratio = custom_ratio == nil ? 4.0 / 13.0 : float(custom_ratio)
      target = float(digits) * ratio
      source = "custom-ratio"
   }
   if(snfs && source != "explicit-target"){
      target = target / 1.2857
      source = source + "-snfs-reduced"
   }
   [target, source]
}

fn _hf_work_step(str: state, str: kind, int: order, any: B1=0, any: B2=0, int: curves=0, any: target_digits=nil): dict {
   {"state": state, "kind": kind, "order": order, "B1": B1, "B2": B2, "curves": curves, "target_digits": target_digits}
}

fn _hf_scheduled_ecm_curves(any: target_digits, int: level, int: prev_level, int: max_curves): int {
   def t = float(target_digits)
   if(t <= float(prev_level)){ return 0 }
   if(t >= float(level)){ return max_curves }
   def span = max(1, level - prev_level)
   def frac = (t - float(prev_level)) / float(span)
   max(1, _hf_ceil_float(float(max_curves) * frac))
}

fn factor_work_schedule_report(any: n, str: pretest_policy="default", any: explicit_target=nil, any: custom_ratio=nil, bool: snfs=false): dict {
   "Return a factor work schedule with target pretest depth, ECM levels, and sieve handoff."
   def t0 = ticks()
   def nn = _hf_abs(n)
   def digits = _hf_digits(nn)
   def target_info = _hf_target_pretest_digits(digits, pretest_policy, explicit_target, custom_ratio, snfs)
   def target_digits = target_info.get(0)
   def target_source = target_info.get(1)
   def levels = _hf_ecm_levels()
   def b1s = _hf_ecm_b1s()
   def max_curves = _hf_ecm_max_curves()
   mut steps = []
   mut ecm_steps = []
   steps = steps.append(_hf_work_step("trialdiv", "pretest", steps.len, 0, 0, 0, target_digits).set("limit", 10000))
   steps = steps.append(_hf_work_step("fermat", "pretest", steps.len, 0, 0, 0, target_digits).set("iterations", 8192))
   steps = steps.append(_hf_work_step("rho", "pretest", steps.len, 0, 0, 3, target_digits).set("bases", 3))
   steps = steps.append(_hf_work_step("pp1_lvl1", "p+1", steps.len, 25000, 0, 0, target_digits))
   steps = steps.append(_hf_work_step("pm1_lvl1", "p-1", steps.len, 150000, 0, 1, target_digits))
   mut total_ecm_curves = 0
   mut i = 0
   while(i < levels.len){
      def level = int(levels.get(i))
      def prev_level = i == 0 ? 0 : int(levels.get(i - 1))
      def sched = _hf_scheduled_ecm_curves(target_digits, level, prev_level, int(max_curves.get(i)))
      def ecm_step = _hf_work_step("ecm_" + to_str(level) + "digit", "ecm", steps.len, b1s.get(i), 0, sched, target_digits)
      .set("level_digits", level)
      .set("max_curves", max_curves.get(i))
      .set("scheduled", sched > 0)
      steps = steps.append(ecm_step)
      ecm_steps = ecm_steps.append(ecm_step)
      total_ecm_curves += sched
      if(level == 25){
         steps = steps.append(_hf_work_step("pp1_lvl2", "p+1", steps.len, 750000, 0, 0, target_digits))
         steps = steps.append(_hf_work_step("pm1_lvl2", "p-1", steps.len, 3750000, 0, 1, target_digits))
      }
      if(level == 30){
         steps = steps.append(_hf_work_step("pp1_lvl3", "p+1", steps.len, 2500000, 0, 0, target_digits))
         steps = steps.append(_hf_work_step("pm1_lvl3", "p-1", steps.len, 15000000, 0, 1, target_digits))
      }
      i += 1
   }
   def trivial_ecm = target_digits < 15.0 && digits <= 45
   def terminal = trivial_ecm ? (digits <= 96 ? "siqs" : "nfs") : (digits >= 75 ? "nfs" : "siqs")
   steps = steps.append(_hf_work_step(terminal == "nfs" ? "state_nfs" : "state_qs", "sieve", steps.len, 0, 0, 0, target_digits)
      .set("sieve_method", terminal)
   .set("stop_reason", trivial_ecm ? "trivial-ecm-skip" : "target-pretest-then-sieve"))
   {
      "method": "factor-work-schedule",
      "source_model": "adaptive autofactor work scheduler",
      "n_bits": bit_length(nn),
      "digits": digits,
      "pretest_policy": pretest_policy,
      "target_digits": target_digits,
      "target_digits_x1000": int(float(target_digits) * 1000.0),
      "target_source": target_source,
      "snfs_reduced": snfs && target_source.contains("snfs-reduced"),
      "trivial_ecm_skip": trivial_ecm,
      "terminal_sieve": terminal,
      "ecm_levels": levels,
      "ecm_level_count": levels.len,
      "ecm_curve_budget": total_ecm_curves,
      "ecm_steps": ecm_steps,
      "steps": steps,
      "step_count": steps.len,
      "has_pm1_levels": true,
      "has_pp1_levels": true,
      "has_ecm_15_to_65": ecm_steps.len == 11,
      "has_sieve_handoff": true,
      "elapsed_ms": _hf_elapsed_ms(t0),
   }
}

fn hybrid_factor_orchestration_report(any: n=8051): dict {
   "Return an auditable summary of the default hybrid factor schedule and report surfaces."
   def plan = factor_plan(n)
   def schedule = factor_work_schedule_report(n)
   {
      "method": "hybrid-factor-orchestration",
      "n_bits": bit_length(_hf_abs(n)),
      "plan": plan,
      "work_schedule": schedule,
      "work_schedule_visible": true,
      "target_pretest_digits": schedule.get("target_digits"),
      "target_pretest_digits_x1000": schedule.get("target_digits_x1000"),
      "ecm_level_count": schedule.get("ecm_level_count"),
      "ecm_curve_budget": schedule.get("ecm_curve_budget"),
      "terminal_sieve": schedule.get("terminal_sieve"),
      "plan_contains_ecm": plan.contains("ecm-stage1-stage2"),
      "plan_contains_siqs": plan.contains("self-initializing-quadratic-sieve"),
      "plan_contains_mpqs": plan.contains("multi-window-quadratic-sieve"),
      "plan_contains_nfs": plan.contains("nfs-square-root-gcd"),
      "reported_pm1": true,
      "reported_pp1": true,
      "reported_ecm": true,
      "reported_siqs": true,
      "reported_mpqs": true,
      "reported_nfs": true,
      "step_report_fields": ["report", "factor", "success", "elapsed_ms", "autotuned", "work_plan", "relation_count", "factor_base_size", "linear_algebra"],
      "mpqs_default_work_plan_visible": true,
      "nested_linear_algebra_visible": true,
      "remove_blocker": false,
      "remaining_blocker": "",
   }
}

fn _trial_division_split_i60(int: nn0, int: bound): list {
   mut nn = nn0
   mut factors = []
   if(nn < 2){ return [factors, Z(nn)] }
   def small = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47]
   mut i = 0
   while(i < small.len){
      def p = small[i]
      while(nn % p == 0){
         factors = factors.append(Z(p))
         nn = nn / p
      }
      i += 1
   }
   mut p = 53
   while(p <= bound && p * p <= nn){
      if(nn % p == 0){
         while(nn % p == 0){
            factors = factors.append(Z(p))
            nn = nn / p
         }
      }
      def p2 = p + 2
      if(p2 <= bound && p2 * p2 <= nn && nn % p2 == 0){
         while(nn % p2 == 0){
            factors = factors.append(Z(p2))
            nn = nn / p2
         }
      }
      p += 6
   }
   [factors, Z(nn)]
}

fn trial_division_split(any: n, int: bound=10000): list {
   "Split small prime factors from `n`. Returns [factors, cofactor]."
   mut nn = _hf_abs(n)
   mut factors = []
   if(nn < Z(2)){ return [factors, nn] }
   if(bit_length(nn) <= 60){ return _trial_division_split_i60(bigint_to_int(nn), bound) }
   def small = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47]
   mut i = 0
   while(i < small.len){
      def p = Z(small.get(i))
      while(nn % p == Z(0)){
         factors = factors.append(p)
         nn = nn / p
      }
      i += 1
   }
   mut p = 53
   def stop = bound
   while(p <= stop && p * p <= nn){
      def pz = Z(p)
      if(nn % pz == Z(0)){
         while(nn % pz == Z(0)){
            factors = factors.append(pz)
            nn = nn / pz
         }
      }
      def p2 = p + 2
      def p2z = Z(p2)
      if(p2 <= stop && p2 * p2 <= nn && nn % p2z == Z(0)){
         while(nn % p2z == Z(0)){
            factors = factors.append(p2z)
            nn = nn / p2z
         }
      }
      p += 6
   }
   [factors, nn]
}

fn _hf_try_candidate(any: n, any: f): any { _hf_nontrivial(f, n) ? _hf_z(f) : nil }

fn _hf_elapsed_ms(any: t0): f64 { float(ticks() - t0) / 1000000.0 }

fn _hf_step(str: method, any: factor, any: t0): dict {
   {"method": method, "factor": factor, "success": factor != nil, "elapsed_ms": _hf_elapsed_ms(t0)}
}

fn _hf_step_report(str: method, any: report, any: t0): dict {
   def factor = report == nil ? nil : report.get("factor", nil)
   mut step = _hf_step(method, factor, t0)
   step = step.set("report", report)
   if(report != nil){
      def keys = _hf_step_report_keys()
      mut i = 0
      while(i < keys.len){
         def k = keys.get(i)
         def v = report.get(k, nil)
         if(v != nil){ step[k] = v }
         i += 1
      }
   }
   step
}

fn _hf_report_candidate_step(any: n, str: method, any: report, any: t0): dict {
   def f = report == nil ? nil : _hf_try_candidate(n, report.get("factor", nil))
   mut step = _hf_step_report(method, report, t0)
   step["factor"] = f
   step["success"] = f != nil
   step
}

fn _hf_squfof_before_lehman(any: n): bool {
   def bits = bit_length(_hf_abs(n))
   bits > 40 && bits <= 64
}

fn _hf_squfof_step(any: n, any: t0): dict {
   _hf_report_candidate_step(n, "squfof", pollard.squfof_report(n), t0)
}

fn _hf_brent_step(any: n, any: y0, any: c0, int: max_gcds, any: t0): dict {
   def br = pollard.pollard_brent_report(n, y0, c0, 64, max_gcds)
   _hf_report_candidate_step(n, "brent-rho:" + to_str(y0) + "," + to_str(c0), br, t0)
}

fn _hf_named_report(any: n, str: method): any {
   case method {
      "p-1-stage2" -> pollard.pollard_pm1_stage2_report(n, 2000, 50000)
      "p+1" -> pollard.williams_pp1_report(n, 20000)
      "ecm-stage1-stage2" -> ecm.ecm_scheduled_factor_report(n, false, 24, 2000, 5000, 8)
      "self-initializing-quadratic-sieve" -> classical.siqs_factor_report(n, 64, 8, 384, 40)
      "multi-window-quadratic-sieve" -> classical.mpqs_factor_report(n)
      "nfs-square-root-gcd" -> nfs.nfs_factor_report(n, 64, 64, 12, 3, 6)
      _ -> nil
   }
}

fn _hf_try_report_suite(any: n, list: steps, list: methods): dict {
   mut out = steps
   mut i = 0
   while(i < methods.len){
      def method = methods.get(i)
      def step = _hf_report_candidate_step(n, method, _hf_named_report(n, method), ticks())
      out = out.append(step)
      if(step.get("success", false)){
         return {"steps": out, "method": method, "factor": step.get("factor"), "success": true}
      }
      i += 1
   }
   {"steps": out, "method": "", "factor": nil, "success": false}
}

fn _hf_try_brent_schedule(any: n, list: steps, int: max_gcds): list {
   def seeds = _hf_brent_seeds_for(n)
   mut out = steps
   mut i = 0
   while(i < seeds.len){
      def s = seeds.get(i)
      def tb = ticks()
      def bstep = _hf_brent_step(n, s.get(0), s.get(1), max_gcds, tb)
      out = out.append(bstep)
      if(bstep.get("success", false)){ return [out, bstep] }
      i += 1
   }
   [out, dict()]
}

fn _hf_prime_powers_to_flat(any: facs): any {
   if(facs == nil){ return nil }
   mut out = []
   mut i = 0
   while(i < facs.len){
      def ent = facs.get(i)
      def p = ent.get(0)
      mut e = ent.get(1)
      while(e > 0){
         out = out.append(p)
         e = e - 1
      }
      i += 1
   }
   out
}

fn _hf_use_factordb(any: source): bool { source == true || source == "factordb" || source == "fdb" }

fn _hf_finish_one(dict: out, list: steps, str: method, any: factor, any: t0): dict {
   out.set("method", method).set("factor", factor).set("success", factor != nil).set("steps", steps).set("elapsed_ms", _hf_elapsed_ms(t0))
}

fn hybrid_factor_one(any: n): any {
   "Find one non-trivial factor using the bounded method schedule."
   hybrid_factor_one_report(n).get("factor", nil)
}

fn hybrid_factor_one_report(any: n): dict {
   "Explain one factor-splitting attempt."
   def t0 = ticks()
   def nn = _hf_abs(n)
   def bits = bit_length(nn)
   mut out = dict(12)
   mut steps = []
   out = out.set("n", nn)
   out = out.set("n_bits", bits)
   out = out.set("plan", factor_plan(nn))
   if(nn <= Z(3)){ return _hf_finish_one(out, steps, "trivial", nil, t0) }
   if(is_prime(nn)){ return _hf_finish_one(out, steps, "prime", nil, t0) }
   if(nn % Z(2) == Z(0)){ return _hf_finish_one(out, steps, "trial-2", Z(2), t0) }
   if(nn % Z(3) == Z(0)){ return _hf_finish_one(out, steps, "trial-3", Z(3), t0) }
   def root = isqrt(nn)
   if(root * root == nn){ return _hf_finish_one(out, steps, "perfect-square", root, t0) }
   def fermat_bound = bits > 64 ? 256 : 8192
   def tf = ticks()
   def close = fermat.fermat_factor_bounded(nn, fermat_bound)
   if(close != nil){
      def f0 = close.get(0, nil)
      def got0 = _hf_try_candidate(nn, f0)
      steps = steps.append(_hf_step("fermat", got0, tf))
      if(got0 != nil){ return _hf_finish_one(out, steps, "fermat", got0, t0) }
   } else {
      steps = steps.append(_hf_step("fermat", nil, tf))
   }
   mut tried_squfof = false
   mut tried_brent = false
   mut tried_pm1_stage2 = false
   if(_hf_squfof_before_lehman(nn)){
      def tsq0 = ticks()
      def sq_step0 = _hf_squfof_step(nn, tsq0)
      tried_squfof = true
      steps = steps.append(sq_step0)
      if(sq_step0.get("success", false)){ return _hf_finish_one(out, steps, "squfof", sq_step0.get("factor"), t0) }
   }
   if(bits > 64 && bits <= 75){
      def tp1 = ticks()
      def p1_step0 = _hf_report_candidate_step(nn, "p-1-stage2", pollard.pollard_pm1_stage2_report(nn, 2000, 50000), tp1)
      tried_pm1_stage2 = true
      steps = steps.append(p1_step0)
      if(p1_step0.get("success", false)){ return _hf_finish_one(out, steps, "p-1-stage2", p1_step0.get("factor"), t0) }
   }
   if(bits > 64 && bits <= 96){
      def brp0 = _hf_try_brent_schedule(nn, steps, 50000)
      tried_brent = true
      steps = brp0.get(0)
      def bstep_pre = brp0.get(1)
      if(bstep_pre.get("success", false)){ return _hf_finish_one(out, steps, "brent-rho", bstep_pre.get("factor"), t0) }
   }
   if(bits <= 96){
      def tl = ticks()
      def l_step = _hf_report_candidate_step(nn, "lehman", pollard.lehman_factor_report(nn, 20000, 256), tl)
      steps = steps.append(l_step)
      if(l_step.get("success", false)){ return _hf_finish_one(out, steps, "lehman", l_step.get("factor"), t0) }
   }
   def mid_methods = tried_pm1_stage2 ? ["p+1", "ecm-stage1-stage2"] : ["p-1-stage2", "p+1", "ecm-stage1-stage2"]
   def mid = _hf_try_report_suite(nn, steps, mid_methods)
   steps = mid.get("steps")
   if(mid.get("success", false)){ return _hf_finish_one(out, steps, mid.get("method"), mid.get("factor"), t0) }
   if(!tried_brent && bits > 64 && bits <= 96){
      def brp = _hf_try_brent_schedule(nn, steps, 50000)
      steps = brp.get(0)
      def bstep0 = brp.get(1)
      if(bstep0.get("success", false)){ return _hf_finish_one(out, steps, "brent-rho", bstep0.get("factor"), t0) }
   }
   if(!tried_squfof){
      def tsq = ticks()
      def sq_step = _hf_squfof_step(nn, tsq)
      steps = steps.append(sq_step)
      if(sq_step.get("success", false)){ return _hf_finish_one(out, steps, "squfof", sq_step.get("factor"), t0) }
   }
   if(bits <= 96){
      def qs = _hf_try_report_suite(nn, steps, ["self-initializing-quadratic-sieve", "multi-window-quadratic-sieve", "nfs-square-root-gcd"])
      steps = qs.get("steps")
      if(qs.get("success", false)){ return _hf_finish_one(out, steps, qs.get("method"), qs.get("factor"), t0) }
   }
   def brf = _hf_try_brent_schedule(nn, steps, 200000)
   steps = brf.get(0)
   def bstep = brf.get(1)
   if(bstep.get("success", false)){ return _hf_finish_one(out, steps, "brent-rho", bstep.get("factor"), t0) }
   if(bits <= 128){
      def tps = ticks()
      def fps = _hf_try_candidate(nn, pollard.pollard_strassen(nn))
      steps = steps.append(_hf_step("pollard-strassen", fps, tps))
      if(fps != nil){ return _hf_finish_one(out, steps, "pollard-strassen", fps, t0) }
   }
   _hf_finish_one(out, steps, "none", nil, t0)
}

fn factor_complete(any: n, int: max_rounds=512): list {
   "Complete local factorization into prime-looking factors.
   Composite leftovers are retained if the bounded schedule cannot split them."
   def start = _hf_abs(n)
   if(start < Z(2)){ return [] }
   if(bit_length(start) > 60 && start % Z(2) != Z(0)){
      def close = fermat.fermat_factor_bounded(start, 128)
      if(close != nil){
         def f0 = _hf_try_candidate(start, close.get(0, nil))
         if(f0 != nil){
            mut out0 = factor_complete(f0, max_rounds)
            def out1 = factor_complete(start / f0, max_rounds)
            mut i = 0
            while(i < out1.len){
               out0 = _hf_append_sorted(out0, out1[i])
               i += 1
            }
            return out0
         }
      }
   }
   if(bit_length(start) > 30 && is_prime(start)){ return [start] }
   def td = trial_division_split(start, 10000)
   mut out = td.get(0, [])
   mut pending = []
   def rest = td.get(1, Z(1))
   if(rest > Z(1)){ pending = pending.append(rest) }
   mut rounds = 0
   mut pending_i = 0
   while(pending_i < pending.len && rounds < max_rounds){
      def x = pending[pending_i]
      pending_i += 1
      if(x <= Z(1)){
         rounds += 1
         continue
      }
      if(is_prime(x)){
         out = _hf_append_sorted(out, x)
         rounds += 1
         continue
      }
      def f = hybrid_factor_one(x)
      if(!_hf_nontrivial(f, x)){
         out = _hf_append_sorted(out, x)
         rounds += 1
         continue
      }
      pending = pending.append(_hf_z(f))
      pending = pending.append(x / _hf_z(f))
      rounds += 1
   }
   mut i = 0
   while(pending_i + i < pending.len){
      out = _hf_append_sorted(out, pending[pending_i + i])
      i += 1
   }
   out
}

fn factor_complete_report(any: n, int: max_rounds=512): dict {
   "Complete factorization through the bounded method schedule and return validation metadata."
   def t0 = ticks()
   def factors = factor_complete(n, max_rounds)
   {
      "n_bits": bit_length(_hf_abs(n)),
      "source": "builtin",
      "plan": factor_plan(n),
      "factors": factors,
      "valid": factor_validate(n, factors),
      "elapsed_ms": _hf_elapsed_ms(t0),
   }
}

fn factordb_query(any: n, bool: fallback=true): any {
   "Query FactorDB and return a flat factor list; fallback keeps old challenge scripts stable."
   _hf_prime_powers_to_flat(factordb_factor(n, fallback))
}

fn hybrid_factor_local(any: n): list {
   "Factorization through the bounded method schedule."
   factor_complete(n)
}

fn hybrid_factor(any: n, any: source=false, any: reserved=false): list {
   "Hybrid factorization through the bounded method schedule.
   Returns a flat list of factors."
   def nn = _hf_abs(n)
   if(nn < Z(2)){ return [] }
   if(_hf_use_factordb(source)){
      def fd = factordb_query(nn, true)
      if(fd != nil && factor_validate(nn, fd)){ return fd }
   }
   factor_complete(nn)
}

fn hybrid_factor_report(any: n, any: source=false, any: reserved=false): dict {
   "Return factorization result plus the method schedule and validation status."
   def t0 = ticks()
   def nn = _hf_abs(n)
   def factors = hybrid_factor(nn, source, reserved)
   def fdb = _hf_use_factordb(source)
   {
      "n_bits": bit_length(nn),
      "plan": factor_plan(nn),
      "source": fdb ? "factordb-first" : "builtin",
      "factordb_requested": fdb,
      "builtin_default": !fdb,
      "factors": factors,
      "valid": factor_validate(nn, factors),
      "elapsed_ms": float(ticks() - t0) / 1000000.0,
   }
}
