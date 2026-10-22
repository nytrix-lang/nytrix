;; Keywords: proof policy obligation certificate tactic solver tri-state laws explanations
;; Ny-authored proof policy for Nytrix.
;;
;; For the core.proof API, this module owns tactic selection and order, solver
;; orchestration, law checking, and human-readable explanations. It proposes
;; versioned certificates for versioned obligations; every certificate is
;; trusted only after the independent bounded C checker
;; (`__proof_policy_check`) re-derives its verdict. Unknown is a distinct,
;; first-class verdict here: it never stands in for false or proved.
;;
;; Compiler proof sites currently use a C-side adapter with the same
;; obligation format; completing the compiler's policy cutover remains open.
;;
;; Interface (versioned, shared with src/code/lang/types/policy.h):
;;   obligation: ny-proof-obligation-v1|prop|ranges|policy|budgets
;;   certificate: ny-proof-certificate-v1|digest|verdict|tactic|backend|cex|explanation
;;     verdict: proved | refuted | unknown
;; References:
;; - core
;; - core.math.logic (tautology-side certificates)
module core.proof(
   OBLIGATION_FORMAT, CERTIFICATE_FORMAT, POLICY_VERSION,
   parse_canonical, canon_eval_closed, canon_decide_ranges,
   tactic_fold, tactic_range, tactic_solver,
   obligation, solve, check, explain, verdict_of, certificate_text,
   law_obligation, law_solve, law_check, law_report, check_texts)
use core
use core.str

def OBLIGATION_FORMAT = "ny-proof-obligation-v1"
def CERTIFICATE_FORMAT = "ny-proof-certificate-v1"
def POLICY_VERSION = "core.proof@1"

;; Checker-visible budgets: mirrored from the compiler's policy defaults.
def MAX_VARS = 8
def MAX_CONS = 32
def MAX_DEPTH = 64

;; Bounded canonical-proposition parser (own parser, own node shape).
;; A node is a dict:
;;   {"kind":"int","value":n} {"kind":"bool","value":b}
;;   {"kind":"name","name":s}
;;   {"kind":"not","value":n} {"kind":"neg","value":n}
;;   {"kind":"bin","op":s,"left":n,"right":n}
;; parse results are {"ok":true,"node":n} or {"ok":false,"reason":s}.
fn _char_at(str s, int i) str {
   _substr(s, i, i + 1)
}

fn _is_digit(str c) bool {
   c == "0" || c == "1" || c == "2" || c == "3" || c == "4" || c == "5" ||
   c == "6" || c == "7" || c == "8" || c == "9"
}

fn _is_name_char(str c) bool {
   (_is_digit(c)) || (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || c == "_"
}

;; Normalizes a bool into 1/0 for dict-literal returns: ternaries inside
;; returned dict literals miscompile through the precompiled stdlib, and a
;; raw bool value would make `!= 0` checks meaningless downstream.
fn _bit(bool b) int {
   if b { return 1 }
   0
}

fn _parse_decimal(str s) dict {
   ;; strict decimal with optional leading '-'
   if len(s) == 0 || len(s) > 20 {
      return {"ok": false, "reason": "bad number length"}
   }
   mut i = 0
   mut neg = false
   if _char_at(s, 0) == "-" {
      neg = true
      i = 1
      if len(s) == 1 { return {"ok": false, "reason": "lone '-'"} }
   }
   mut v = 0
   while i < len(s) {
      def c = _char_at(s, i)
      if !_is_digit(c) { return {"ok": false, "reason": "bad digit"} }
      v = v * 10 + (to_int(c) - to_int("0"))
      i += 1
   }
   ;; Keep the sign fixup off the dict-literal line: a ternary inside a
   ;; returned literal miscompiles through the precompiled stdlib.
   mut result = v
   if neg { result = -v }
   {"ok": true, "value": result}
}

@noinline
fn _find_top_level_comma(str s) int {
   ;; returns index of the first depth-0 ',' or -1
   mut depth = 0
   mut i = 0
   while i < len(s) {
      def c = _char_at(s, i)
      if c == "(" {
         depth += 1
      } elif c == ")" {
         if depth == 0 { return -1 }
         depth -= 1
      } elif c == "," && depth == 0 {
         return i
      }
      i += 1
   }
   -1
}

@noinline
fn _parse_node(str s, int depth) dict {
   if depth > 40 {
      return {"ok": false, "reason": "depth budget exhausted"}
   }
   if len(s) == 0 {
      return {"ok": false, "reason": "empty node text"}
   }
   if startswith(s, "name:") {
      def body = slice(s, 5, len(s))
      if len(body) == 0 || len(body) > 63 {
         return {"ok": false, "reason": "bad name length"}
      }
      mut i = 0
      while i < len(body) {
         if !_is_name_char(_char_at(body, i)) {
            return {"ok": false, "reason": "bad name character"}
         }
         i += 1
      }
      return {"ok": true, "node": {"kind": "name", "name": body}}
   }
   if startswith(s, "int:") {
      def parsed = _parse_decimal(slice(s, 4, len(s)))
      if !parsed.get("ok") {
         return {"ok": false, "reason": "bad int literal"}
      }
      return {"ok": true,
         "node": {"kind": "int", "value": parsed.get("value")}}
   }
   if s == "bool:true" {
      return {"ok": true, "node": {"kind": "bool", "value": true}}
   }
   if s == "bool:false" {
      return {"ok": true, "node": {"kind": "bool", "value": false}}
   }
   if startswith(s, "unary:!(") {
      if len(s) < 10 || slice(s, len(s) - 1, len(s)) != ")" {
         return {"ok": false, "reason": "unbalanced negation"}
      }
      def inner = _parse_node(slice(s, 8, len(s) - 1), depth + 1)
      if !inner.get("ok") { return inner }
      return {"ok": true, "node": {"kind": "not", "value": inner.get("node")}}
   }
   if startswith(s, "unary:-(") {
      if len(s) < 10 || slice(s, len(s) - 1, len(s)) != ")" {
         return {"ok": false, "reason": "unbalanced negation"}
      }
      def inner = _parse_node(slice(s, 8, len(s) - 1), depth + 1)
      if !inner.get("ok") { return inner }
      return {"ok": true, "node": {"kind": "neg", "value": inner.get("node")}}
   }
   if startswith(s, "binary:") {
      if len(s) < 9 {
         return {"ok": false, "reason": "short binary node"}
      }
      if slice(s, len(s) - 1, len(s)) != ")" {
         return {"ok": false, "reason": "binary node lacks ')'"}
      }
      def open_at = find_from(s, "(", 7)
      if open_at < 0 {
         return {"ok": false, "reason": "binary node lacks '('"}
      }
      def op = slice(s, 7, open_at)
      if len(op) == 0 || len(op) > 3 {
         return {"ok": false, "reason": "bad operator length"}
      }
      def body = slice(s, open_at + 1, len(s) - 1)
      def comma = _find_top_level_comma(body)
      if comma < 0 {
         return {"ok": false, "reason": "binary node lacks ','"}
      }
      def left = _parse_node(slice(body, 0, comma), depth + 1)
      if !left.get("ok") { return left }
      def right = _parse_node(slice(body, comma + 1, len(body)), depth + 1)
      if !right.get("ok") { return right }
      return {"ok": true,
         "node": {"kind": "bin", "op": op, "left": left.get("node"),
            "right": right.get("node")}}
   }
   {"ok": false, "reason": "unsupported proposition token"}
}

;; Parses a canonical proposition string under a fixed depth budget.
@noinline
fn parse_canonical(str text) dict {
   ;; Guard the interpreter's small-string fast path: canonical text past
   ;; this size is out of budget for the Ny tactics anyway, and oversized
   ;; strings take representation paths the bounded parser must not walk.
   if len(text) > 2000 {
      return {"ok": false, "reason": "proposition exceeds the policy budget"}
   }
   _parse_node(text, 0)
}

;; Tactic: fold - exact evaluation of closed propositions in Ny.
;; Returns {"ok":true,"value":int} (1/0 for propositions) or
;; {"ok":false,"reason":s} when the tree is not closed/evaluable.
@noinline
fn _fold_bin(str op, int l, int r) dict {
   ;; Operators only: the band/bor/bxor/add/sub/mul wrapper functions do
   ;; not survive the precompiled-stdlib path for dynamic operands, and a
   ;; ternary inside a returned dict literal miscompiles the same way.
   if op == "<" { return {"ok": true, "value": _bit(l < r)} }
   if op == "<=" { return {"ok": true, "value": _bit(l <= r)} }
   if op == ">" { return {"ok": true, "value": _bit(l > r)} }
   if op == ">=" { return {"ok": true, "value": _bit(l >= r)} }
   if op == "==" { return {"ok": true, "value": _bit(l == r)} }
   if op == "!=" { return {"ok": true, "value": _bit(l != r)} }
   if op == "&" { return {"ok": true, "value": l & r} }
   if op == "|" { return {"ok": true, "value": l | r} }
   if op == "^" { return {"ok": true, "value": l ^ r} }
   if op == "<<" {
      if r < 0 || r >= 64 { return {"ok": false, "reason": "shift width"} }
      return {"ok": true, "value": l << r}
   }
   if op == ">>" {
      if r < 0 || r >= 64 { return {"ok": false, "reason": "shift width"} }
      return {"ok": true, "value": l >> r}
   }
   if op == "/" || op == "%" {
      if r == 0 { return {"ok": false, "reason": "division by zero"} }
      if op == "/" { return {"ok": true, "value": l / r} }
      return {"ok": true, "value": l % r}
   }
   if op == "+" { return {"ok": true, "value": l + r} }
   if op == "-" { return {"ok": true, "value": l - r} }
   if op == "*" { return {"ok": true, "value": l * r} }
   {"ok": false, "reason": "unsupported operator"}
}

@noinline
fn _closed_eval(dict node, int depth) dict {
   if depth > 40 {
      return {"ok": false, "reason": "depth budget exhausted"}
   }
   def kind = node.get("kind")
   if kind == "int" || kind == "bool" {
      if kind == "bool" {
         return {"ok": true, "value": _bit(node.get("value"))}
      }
      return {"ok": true, "value": node.get("value")}
   }
   if kind == "name" {
      return {"ok": false, "reason": "not closed: name '" +
         node.get("name") + "'"}
   }
   if kind == "not" {
      def inner = _closed_eval(node.get("value"), depth + 1)
      if !inner.get("ok") { return inner }
      return {"ok": true, "value": _bit(inner.get("value") == 0)}
   }
   if kind == "neg" {
      def inner = _closed_eval(node.get("value"), depth + 1)
      if !inner.get("ok") { return inner }
      return {"ok": true, "value": -inner.get("value")}
   }
   ;; bin
   def left = _closed_eval(node.get("left"), depth + 1)
   if !left.get("ok") { return left }
   def right = _closed_eval(node.get("right"), depth + 1)
   if !right.get("ok") { return right }
   def op = node.get("op")
   if op == "&&" {
      def both = left.get("value") != 0 && right.get("value") != 0
      return {"ok": true, "value": _bit(both)}
   }
   if op == "||" {
      def either = left.get("value") != 0 || right.get("value") != 0
      return {"ok": true, "value": _bit(either)}
   }
   _fold_bin(op, left.get("value"), right.get("value"))
}

;; Fold tactic: decide a proposition that mentions no names at all.
;; Returns {"decided":true,"value":bool} or {"decided":false,"reason":s}.
fn tactic_fold(dict proposition) dict {
   def parsed = parse_canonical(proposition.get("canonical", ""))
   if !parsed.get("ok") {
      return {"decided": false, "reason": parsed.get("reason")}
   }
   def result = _closed_eval(parsed.get("node"), 0)
   if !result.get("ok") {
      return {"decided": false, "reason": result.get("reason")}
   }
   return {"decided": true, "value": result.get("value") != 0}
}

;; Tactic: range - interval propagation over the obligation's ranges.
;; Parses "name=lo..hi;name2=lo..hi" into {"name":[lo,hi], ...}.
fn parse_ranges(str ranges) dict {
   mut out = {}
   if len(ranges) == 0 {
      return out
   }
   def entries = _split_char(ranges, ";")
   mut i = 0
   while i < len(entries) {
      def entry = get(entries, i)
      def eq = _find_char(entry, "=")
      if eq < 0 {
         return {}
      }
      def name = slice(entry, 0, eq)
      def dots = _find_from_char(entry, "..", eq + 1)
      if dots < 0 {
         return {}
      }
      def lo_p = _parse_decimal(slice(entry, eq + 1, dots))
      def hi_p = _parse_decimal(slice(entry, dots + 2, len(entry)))
      if !lo_p.get("ok") || !hi_p.get("ok") {
         return {}
      }
      if lo_p.get("value") > hi_p.get("value") {
         return {}
      }
      out = out.set(name, [lo_p.get("value"), hi_p.get("value")])
      i += 1
   }
   out
}

fn _split_char(str s, str sep) list {
   mut out = []
   mut start = 0
   while true {
      def at = find_from(s, sep, start)
      if at < 0 {
         out = out.append(slice(s, start, len(s)))
         break
      }
      out = out.append(slice(s, start, at))
      start = at + 1
   }
   out
}

fn _find_char(str s, str sub) int {
   find_from(s, sub, 0)
}

fn _find_from_char(str s, str sub, int start) int {
   find_from(s, sub, start)
}

;; Interval of a subtree under known ranges; {"ok":bool,"lo":int,"hi":int}.
@noinline
fn _sub_range(dict node, dict ranges, int depth) dict {
   if depth > 40 {
      return {"ok": false, "lo": 0, "hi": 0}
   }
   def kind = node.get("kind")
   if kind == "int" {
      def v = node.get("value")
      return {"ok": true, "lo": v, "hi": v}
   }
   if kind == "name" {
      def r = ranges.get(node.get("name"), nil)
      if r == nil {
         return {"ok": false, "lo": 0, "hi": 0}
      }
      return {"ok": true, "lo": get(r, 0), "hi": get(r, 1)}
   }
   if kind == "neg" {
      def inner = _sub_range(node.get("value"), ranges, depth + 1)
      return {"ok": inner.get("ok"), "lo": -(inner.get("hi")),
         "hi": -(inner.get("lo"))}
   }
   if kind == "bin" {
      def l = _sub_range(node.get("left"), ranges, depth + 1)
      def r = _sub_range(node.get("right"), ranges, depth + 1)
      if !l.get("ok") || !r.get("ok") {
         return {"ok": false, "lo": 0, "hi": 0}
      }
      def op = node.get("op")
      ;; Plain operators and direct literals here: the add/sub/mul wrapper
      ;; functions and set-chained result dicts do not survive the
      ;; precompiled-stdlib path for dynamic operands.
      def l_lo = l.get("lo")
      def l_hi = l.get("hi")
      def r_lo = r.get("lo")
      def r_hi = r.get("hi")
      if op == "+" {
         return {"ok": true, "lo": l_lo + r_lo, "hi": l_hi + r_hi}
      }
      if op == "-" {
         return {"ok": true, "lo": l_lo - r_hi, "hi": l_hi - r_lo}
      }
      if op == "*" {
         def a = l_lo * r_lo
         def b = l_lo * r_hi
         def c = l_hi * r_lo
         def d = l_hi * r_hi
         mut lo = a
         mut hi = a
         if b < lo { lo = b }
         if c < lo { lo = c }
         if d < lo { lo = d }
         if b > hi { hi = b }
         if c > hi { hi = c }
         if d > hi { hi = d }
         return {"ok": true, "lo": lo, "hi": hi}
      }
      return {"ok": false, "lo": 0, "hi": 0}
   }
   {"ok": false, "lo": 0, "hi": 0}
}

;; Range tactic: interval decision over a parsed proposition.  Sound and
;; deliberately incomplete: intervals that do not decide leave the
;; proposition unknown so the solver tactics can still take it.
;; Returns {"decided":true,"value":bool} or {"decided":false,"reason":s}.
fn tactic_range(dict proposition) dict {
   def parsed = parse_canonical(proposition.get("canonical", ""))
   if !parsed.get("ok") {
      return {"decided": false, "reason": parsed.get("reason")}
   }
   def ranges = parse_ranges(proposition.get("ranges", ""))
   def decision = _range_decide(parsed.get("node"), ranges, 0)
   decision
}

@noinline
fn _range_decide(dict node, dict ranges, int depth) dict {
   if depth > 40 {
      return {"decided": false, "reason": "depth budget exhausted"}
   }
   def kind = node.get("kind")
   if kind == "bool" {
      return {"decided": true, "value": node.get("value")}
   }
   if kind == "int" {
      return {"decided": true, "value": node.get("value") != 0}
   }
   if kind == "name" {
      return {"decided": false, "reason": "bare name is not a proposition"}
   }
   if kind == "not" {
      def inner = _range_decide(node.get("value"), ranges, depth + 1)
      if !inner.get("decided") { return inner }
      return {"decided": true, "value": !inner.get("value")}
   }
   def op = node.get("op")
   if op == "&&" {
      def l = _range_decide(node.get("left"), ranges, depth + 1)
      def r = _range_decide(node.get("right"), ranges, depth + 1)
      if !l.get("decided") || !r.get("decided") {
         return {"decided": false, "reason": "conjunct undecided"}
      }
      return {"decided": true, "value": l.get("value") && r.get("value")}
   }
   if op == "||" {
      def l = _range_decide(node.get("left"), ranges, depth + 1)
      if l.get("decided") && l.get("value") {
         return {"decided": true, "value": true}
      }
      def r = _range_decide(node.get("right"), ranges, depth + 1)
      if !l.get("decided") || !r.get("decided") {
         return {"decided": false, "reason": "disjunct undecided"}
      }
      return {"decided": true, "value": l.get("value") || r.get("value")}
   }
   def l = _sub_range(node.get("left"), ranges, depth + 1)
   def r = _sub_range(node.get("right"), ranges, depth + 1)
   if !l.get("ok") || !r.get("ok") {
      return {"decided": false, "reason": "operand ranges unknown"}
   }
   if op == "<" {
      if l.get("hi") < r.get("lo") { return {"decided": true, "value": true} }
      if l.get("lo") >= r.get("hi") {
         return {"decided": true, "value": false}
      }
   } elif op == "<=" {
      if l.get("hi") <= r.get("lo") {
         return {"decided": true, "value": true}
      }
      if l.get("lo") > r.get("hi") {
         return {"decided": true, "value": false}
      }
   } elif op == ">" {
      if l.get("lo") > r.get("hi") {
         return {"decided": true, "value": true}
      }
      if l.get("hi") <= r.get("lo") {
         return {"decided": true, "value": false}
      }
   } elif op == ">=" {
      if l.get("lo") >= r.get("hi") {
         return {"decided": true, "value": true}
      }
      if l.get("hi") < r.get("lo") {
         return {"decided": true, "value": false}
      }
   } elif op == "==" {
      if l.get("lo") == l.get("hi") && r.get("lo") == r.get("hi") &&
      l.get("lo") == r.get("lo") {
         return {"decided": true, "value": true}
      }
      if l.get("hi") < r.get("lo") || r.get("hi") < l.get("lo") {
         return {"decided": true, "value": false}
      }
   } elif op == "!=" {
      if l.get("hi") < r.get("lo") || r.get("hi") < l.get("lo") {
         return {"decided": true, "value": true}
      }
      if l.get("lo") == l.get("hi") && r.get("lo") == r.get("hi") &&
      l.get("lo") == r.get("lo") {
         return {"decided": true, "value": false}
      }
   } else {
      return {"decided": false, "reason": "non-comparison proposition"}
   }
   {"decided": false, "reason": "ranges do not decide the proposition"}
}

;; Tactic: solver - bounded kernels over the canonical obligation.
;; The C kernel answers "proved" | "refuted:<bindings>" | "unknown:<...>";
;; unknown arrives here as a real verdict and is never collapsed.
fn tactic_solver(dict proposition, str mode) dict {
   def answer = __proof_policy_solve(proposition.get("canonical", ""),
      proposition.get("ranges", ""), mode, MAX_VARS, MAX_CONS, MAX_DEPTH)
   if answer == "proved" {
      return {"verdict": "proved", "backend": mode, "cex": "", "reason": ""}
   }
   if startswith(answer, "refuted:") {
      return {"verdict": "refuted", "backend": mode,
         "cex": slice(answer, 8, len(answer)), "reason": ""}
   }
   mut reason = "no backend could decide"
   if startswith(answer, "unknown:") {
      reason = slice(answer, 8, len(answer))
   }
   {"verdict": "unknown", "backend": mode, "cex": "", "reason": reason}
}

;; Obligations and certificates
;; Field escaping: '|' separates fields and '%' introduces escapes, so both
;; are percent-encoded inside every field (mirrors src/code/lang/types/policy.c).
fn _field_escape(str s) str {
   replace(replace(s, "%", "%25"), "|", "%7c")
}

;; Builds a versioned obligation record.  `ranges` is "" or
;; "name=lo..hi;..." (sorted by name for digest stability); `policy` is a
;; comma-joined tactic list, "" meaning the default order.
fn obligation(str canonical, str ranges="", str policy="") dict {
   def budgets = "v" + to_str(MAX_VARS) + ",c" + to_str(MAX_CONS) + ",d" +
   to_str(MAX_DEPTH)
   def text = OBLIGATION_FORMAT + "|" + _field_escape(canonical) + "|" +
   _field_escape(ranges) + "|" + _field_escape(policy) + "|" + budgets
   ;; Build with set(): dict literals with large dynamic members materialize
   ;; through pooled storage and hand back corrupted strings.
   mut out = {}
   out = out.set("format", OBLIGATION_FORMAT)
   out = out.set("canonical", canonical)
   out = out.set("ranges", ranges)
   out = out.set("policy", policy)
   out = out.set("budgets", budgets)
   out = out.set("text", text)
   out = out.set("digest", __proof_policy_digest_hex(text))
   out
}

fn _well_formed_obligation(dict ob) dict {
   if !is_dict(ob) {
      return {"ok": false, "reason": "obligation is not a record"}
   }
   if ob.get("format", "") != OBLIGATION_FORMAT {
      return {"ok": false, "reason": "obligation format mismatch"}
   }
   if !is_str(ob.get("canonical", nil)) || ob.get("canonical") == "" {
      return {"ok": false, "reason": "obligation proposition missing"}
   }
   if !is_str(ob.get("ranges", nil)) || !is_str(ob.get("policy", nil)) ||
   !is_str(ob.get("text", nil)) {
      return {"ok": false, "reason": "obligation fields missing"}
   }
   {"ok": true, "reason": ""}
}

;; Renders a certificate; the caller passes the verdict fields explicitly.
fn _certificate(dict ob, str verdict, str tactic, str backend, str cex,
   str explanation) dict {
   def text = CERTIFICATE_FORMAT + "|" + ob.get("digest") + "|" + verdict +
   "|" + _field_escape(tactic) + "|" + _field_escape(backend) + "|" +
   _field_escape(cex) + "|" + _field_escape(explanation)
   mut out = {}
   out = out.set("format", CERTIFICATE_FORMAT)
   out = out.set("obligation_text", ob.get("text"))
   out = out.set("digest", ob.get("digest"))
   out = out.set("verdict", verdict)
   out = out.set("tactic", tactic)
   out = out.set("backend", backend)
   out = out.set("cex", cex)
   out = out.set("explanation", explanation)
   out = out.set("text", text)
   out
}

;; Certificate text accessor (for cross-module exchange).
fn certificate_text(dict certificate) str {
   certificate.get("text", "")
}

;; Verdict accessor: "proved" | "refuted" | "unknown".
fn verdict_of(dict certificate) str {
   certificate.get("verdict", "unknown")
}

;; Orchestration: solve an obligation under the Ny-owned policy.
fn solve(dict ob) dict {
   def shape = _well_formed_obligation(ob)
   if !shape.get("ok") {
      ;; Fail closed: a malformed obligation can only yield unknown, and
      ;; the independent checker will reject any stronger claim.
      return _certificate(ob, "unknown", "policy", "core.proof", "",
         "malformed obligation: " + shape.get("reason"))
   }
   ;; Policy: tactic order is fold, range, solver.  Tokens default to the
   ;; full order; "solver" means both bounded kernels in auto mode.
   def policy = ob.get("policy", "")
   def wants_fold = policy == "" || _policy_has(policy, "fold")
   def wants_range = policy == "" || _policy_has(policy, "range")
   wants_presburger = policy == "" || _policy_has(policy, "solver") ||
   _policy_has(policy, "kernels") || _policy_has(policy, "auto") ||
   _policy_has(policy, "presburger")
   wants_z3 = policy == "" || _policy_has(policy, "solver") ||
   _policy_has(policy, "kernels") || _policy_has(policy, "auto") ||
   _policy_has(policy, "z3")
   if wants_fold {
      def fold = tactic_fold(ob)
      if fold.get("decided") {
         if fold.get("value") {
            return _certificate(ob, "proved", "fold", "core.proof", "",
               "proved by exact evaluation of a closed proposition")
         }
         return _certificate(ob, "refuted", "fold", "core.proof", "",
            "proposition is constant-false under exact evaluation")
      }
   }
   if wants_range {
      def range_decision = tactic_range(ob)
      if range_decision.get("decided") {
         if range_decision.get("value") {
            return _certificate(ob, "proved", "range", "core.proof", "",
               "proved by interval propagation over the obligation ranges")
         }
         return _certificate(ob, "refuted", "range", "core.proof", "",
            "refuted by interval propagation over the obligation ranges")
      }
   }
   mut verdict = "unknown"
   mut tactic = "solver"
   mut backend = ""
   mut cex = ""
   mut reason = "no backend could decide"
   if wants_presburger {
      def pres = tactic_solver(ob, "presburger")
      verdict = pres.get("verdict")
      backend = pres.get("backend")
      cex = pres.get("cex")
      reason = pres.get("reason")
   }
   if wants_z3 && verdict == "unknown" {
      def z3 = tactic_solver(ob, "z3")
      if z3.get("verdict") == "proved" {
         verdict = "proved"
         backend = z3.get("backend")
      } elif z3.get("verdict") == "refuted" {
         if verdict == "refuted" {
            ;; Presburger refuted and Z3 refuted: coherent.
            verdict = "refuted"
            backend = z3.get("backend")
            cex = z3.get("cex")
         } else {
            ;; One backend refutes while no backend proves: report the
            ;; refutation; the checker verifies the counterexample anyway.
            verdict = "refuted"
            backend = z3.get("backend")
            cex = z3.get("cex")
         }
      }
   }
   if verdict == "refuted" && len(cex) == 0 {
      ;; A refutation without bindings cannot be verified by the checker;
      ;; downgrade to unknown rather than claim evidence we lack.
      verdict = "unknown"
      reason = "refutation lacked a verifiable counterexample"
   }
   if verdict == "proved" {
      return _certificate(ob, "proved", tactic,
         backend == "" ? "kernels" : backend, "",
         "proved by the bounded kernels: negation unsatisfiable")
   }
   if verdict == "refuted" {
      return _certificate(ob, "refuted", tactic, backend, cex,
         "refuted: counterexample verified by exact evaluation")
   }
   if verdict == "unknown" && wants_range &&
   ob.get("canonical", "").contains("binary:*(") {
      ;; The independent checker has bounded interval rules beyond the Ny
      ;; range propagator. Ask it to validate a candidate proof; only return
      ;; that certificate when the trust boundary independently derives it.
      def interval_candidate = _certificate(ob, "proved", "interval", "checker",
         "", "candidate bounded interval proof")
      if check(interval_candidate) {
         return interval_candidate
      }
   }
   _certificate(ob, "unknown", tactic, backend, "", reason)
}

fn _policy_has(str policy, str token) bool {
   def parts = _split_char(policy, ",")
   mut i = 0
   while i < len(parts) {
      if get(parts, i) == token {
         return true
      }
      i += 1
   }
   false
}

;; Trust boundary: only the independent C checker decides.
fn check(dict certificate) bool {
   if !is_dict(certificate) {
      return false
   }
   if !is_str(certificate.get("text", nil)) ||
   !is_str(certificate.get("obligation_text", nil)) {
      return false
   }
   __proof_policy_check(certificate.get("obligation_text"),
      certificate.get("text"))
}

;; Raw two-text form of the trust boundary, for code that carries the
;; versioned texts directly (exchange, storage, mutation testing).
fn check_texts(str obligation_text, str certificate_text) bool {
   __proof_policy_check(obligation_text, certificate_text)
}

;; Explanations
;; Renders a human-readable explanation for a certificate.  Unknown is
;; explained as "no decision", never as false.
fn explain(dict certificate) str {
   if !is_dict(certificate) {
      return "no certificate"
   }
   def verdict = certificate.get("verdict", "unknown")
   def backend = certificate.get("backend", "")
   def tactic = certificate.get("tactic", "")
   if verdict == "proved" {
      return "proved(" + tactic + " via " + backend + "): " +
      certificate.get("explanation", "")
   }
   if verdict == "refuted" {
      def cex = certificate.get("cex", "")
      return "refuted(" + tactic + " via " + backend + "): counterexample " +
      (len(cex) > 0 ? cex : "n/a")
   }
   "unknown(" + tactic + "): " + certificate.get("explanation",
      "no decision was reached") + " - the obligation is open, not false"
}

;; Laws
;; Builds the obligation for one law of an impl.  Laws are universally
;; quantified over their named operands; the obligation carries the same
;; encoding the compiler funnel produces.
fn law_obligation(str trait_name, str type_name, str canonical,
   str ranges="") dict {
   obligation(canonical, ranges, "solver")
}

;; Solves a law obligation and returns {"law":..., "certificate":...}.
fn law_solve(str trait_name, str type_name, dict ob) dict {
   {"trait": trait_name, "type": type_name, "certificate": solve(ob)}
}

;; True when the law certificate survives the independent checker.
fn law_check(dict law_record) bool {
   if !is_dict(law_record) || !is_dict(law_record.get("certificate", nil)) {
      return false
   }
   check(law_record.get("certificate"))
}

;; Batch report: {"trait":..., "type":..., "laws":[...]} where each law is
;; {"canonical":..., "verdict":..., "explanation":...}.  Unknown laws are
;; reported as open; they never fail as false here - policy code decides.
fn law_report(str trait_name, str type_name, list law_canonicals,
   list law_ranges) dict {
   mut laws = []
   mut i = 0
   while i < len(law_canonicals) {
      def ranges = i < len(law_ranges) ? get(law_ranges, i) : ""
      def ob = law_obligation(trait_name, type_name, get(law_canonicals, i),
         ranges)
      def certificate = solve(ob)
      laws = laws.append({"canonical": get(law_canonicals, i),
            "verdict": certificate.get("verdict"),
            "explanation": explain(certificate)})
      i += 1
   }
   {"trait": trait_name, "type": type_name, "laws": laws}
}
