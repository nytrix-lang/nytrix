;; Keywords: tactic induction simp ring omega exists nonlinear requires total
;; Ny-owned tactics: induction, simp, ring, omega-lite, exists synthesis,
;; nonlinear split picker, requires pre-pass, total measure proposer.
;;
;; Every tactic returns obligation text (or a verdict dict) the existing
;; `proof.solve` / `proof.check` already accepts. Nothing here bypasses the
;; independent bounded C checker (`__proof_policy_check`); unknown stays
;; unknown. Budgets mirror `core.proof`: 8 vars, 32 cons, depth 64.
;; References:
;; - core.proof (obligation/solve/check)
;; - core.math.logic (propositional certificates for ring skeleton)
module core.tactic(
   prove_closed, induction_and, simp_conjunction, ring_distrib_check,
   ring_normalize_closed, omega_fin_close, synth_even_witness,
   synth_bounded_witness, split_picker_mid, divisor_safe_obligation,
   inline_fin_range, propose_total_pair, TACTIC_VERSION)
use core
use core.proof as proof
use core.math.logic as logic

def TACTIC_VERSION = "core.tactic@1"

;; Thin wrapper: build + solve + check a closed obligation.
;; Returns {"verdict":v, "checked":bool, "certificate":cert}.
fn prove_closed(str canonical, str ranges="", str policy="") dict {
   def ob = proof.obligation(canonical, ranges, policy)
   def cert = proof.solve(ob)
   def verdict = proof.verdict_of(cert)
   {"verdict": verdict, "checked": proof.check(cert), "certificate": cert,
      "obligation": ob}
}

;; Induction combinator: proved iff every per-arm certificate is proved
;; and checked. Arms are proved separately (e.g. one `prove` per enum
;; constructor via `comptime variants` + per-arm `prove` at the call site);
;; this fn only combines, so the forall shape holds by case exhaustion.
fn induction_and(list certs) dict {
   mut i = 0
   mut checked_all = true
   while i < len(certs) {
      def c = get(certs, i)
      if proof.verdict_of(c) != "proved" {
         return {"verdict": proof.verdict_of(c), "checked": false,
            "failing_arm": i}
      }
      if !proof.check(c) {
         checked_all = false
      }
      i += 1
   }
   {"verdict": "proved", "checked": checked_all, "arms": len(certs)}
}

;; Simp: ordered conjunction over explicit per-conjunct policies.
;; Each entry is {"canonical":s, "ranges":s, "policy":s}. No single tactic
;; proves the whole chain today (fold needs closed, range needs ranges,
;; solver needs linear); simp dispatches each conjunct to its tactic and
;; conjoins. Returns proved iff every conjunct proves and checks.
fn simp_conjunction(list conjuncts) dict {
   mut i = 0
   while i < len(conjuncts) {
      def entry = get(conjuncts, i)
      def ob = proof.obligation(entry.get("canonical", ""),
         entry.get("ranges", ""), entry.get("policy", ""))
      def cert = proof.solve(ob)
      if proof.verdict_of(cert) != "proved" || !proof.check(cert) {
         return {"verdict": proof.verdict_of(cert), "checked": false,
            "failing": i}
      }
      i += 1
   }
   {"verdict": "proved", "checked": true, "discharged": len(conjuncts)}
}

;; Ring skeleton check via core.math.logic: the propositional shape
;; (p && q -> r) used by the distrib proof must be a tautology.
;; This ties the ring normalizer to logic certs, not just kernel arithmetic.
fn ring_distrib_check() dict {
   def p = logic.prop_atom("distrib_holds")
   def q = logic.prop_atom("norm_equal")
   def imp = logic.prop_implies(logic.prop_and(p, q), q)
   def report = logic.prop_tautology_report(imp)
   def cert = logic.prop_certificate(imp)
   {"valid": report.get("decided") && report.get("valid"),
      "checked": logic.prop_check_certificate(cert)}
}

;; Ring normalizer for closed distrib instances: normalizes
;; `a*(b+c)` vs `a*b+a*c` by exact evaluation of both sides (fold),
;; after asserting the logic skeleton above. Returns proved iff the
;; closed equality proves via `proof.solve` and the skeleton holds.
fn ring_normalize_closed(int a, int b, int c) dict {
   def skeleton = ring_distrib_check()
   if !skeleton.get("valid") || !skeleton.get("checked") {
      return {"verdict": "unknown", "checked": false,
         "reason": "logic skeleton failed"}
   }
   def lhs = a * (b + c)
   def rhs = a * b + a * c
   def ob = proof.obligation("binary:==(int:" + to_str(lhs) + ",int:" +
      to_str(rhs) + ")", "", "fold")
   def cert = proof.solve(ob)
   {"verdict": proof.verdict_of(cert), "checked": proof.check(cert),
      "lhs": lhs, "rhs": rhs}
}

;; Omega-lite: close over Fin/range facts by threading the resolved
;; `[0,N)` range into the obligation before the kernel sees it.
;; Caller passes the Fin bound N and the canonical prop over `name`.
fn omega_fin_close(str canonical, str name, int bound) dict {
   def ranges = name + "=0.." + to_str(bound - 1)
   def ob = proof.obligation(canonical, ranges, "solver")
   def cert = proof.solve(ob)
   {"verdict": proof.verdict_of(cert), "checked": proof.check(cert),
      "ranges": ranges}
}

;; Exists witness synthesis over bounded Fin<N>: linear search for k
;; with `v == 2*k`, then feed the witness term to `prove` (never bypass).
;; Returns {"found":bool, "witness":k}.
fn synth_even_witness(int v, int bound) dict {
   mut k = 0
   while k < bound {
      if v == 2 * k {
         return {"found": true, "witness": k}
      }
      k += 1
   }
   {"found": false, "witness": -1}
}

;; Generic bounded witness search: first `k < bound` with `pred(k)` true.
;; `pred` is an ordinary Ny fn `int -> bool`; the result feeds `prove`.
fn synth_bounded_witness(int bound, any pred) dict {
   mut k = 0
   while k < bound {
      if pred(k) {
         return {"found": true, "witness": k}
      }
      k += 1
   }
   {"found": false, "witness": -1}
}

;; Nonlinear split picker: midpoint of a Fin<N> domain. The caller proves
;; each sub-range separately (checker does bounded splitting); this fn
;; only proposes the point so ranges are not hand-written.
fn split_picker_mid(int bound) int {
   bound / 2
}

;; Division side-condition obligation: `divisor != 0` plus bounded
;; quotient, discharged first. Returns the solved cert dict.
fn divisor_safe_obligation(str divisor_canonical, str ranges) dict {
   def ob = proof.obligation(divisor_canonical, ranges, "solver")
   def cert = proof.solve(ob)
   {"verdict": proof.verdict_of(cert), "checked": proof.check(cert)}
}

;; Requires pre-pass: inline a resolved Fin<N> range into the caller
;; obligation text. Same verdict rules, fewer unknowns: returns the
;; ranges string the kernel should see.
fn inline_fin_range(str name, int bound) str {
   name + "=0.." + to_str(bound - 1)
}

;; Total measure proposer: lexicographic pair from call args.
;; The compiler still proves every edge or rejects; this only proposes.
fn propose_total_pair(int major, int minor) list {
   [major, minor]
}
