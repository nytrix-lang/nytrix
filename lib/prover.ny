;; Keywords: prover goal tactic hypothesis context term certificate search
;; A small goal-directed prover written in Nytrix over core.proof.
;;
;; Terms are core.proof proposition nodes; `term` reads source spelling
;; (`x + 1 > 0 && y == x`) and `show`/`canon` print it back. A goal is a
;; context (hypotheses), a target, and variable ranges. A proof state is a
;; list of open goals, the certificates that closed the others, and a log.
;;
;; Trust: this library is not a kernel. A goal closes only through `decide`,
;; which builds the obligation `hyps -> target` and keeps the certificate only
;; when core.proof proposes it *and* the independent C checker re-derives it.
;; Goal-splitting rules come in two kinds:
;;  - the fixed structural rules here (split_conj, intro, cases,
;;    subst, split_range), each a standard sound rule of the sequent calculus over
;;    the bounded integer domain;
;;  - user tactics: any `fn(any goal) dict` run with `by`. Their output is
;;    not trusted: `by` accepts the new goals only after the checker proves
;;    that they entail the old one, so a wrong tactic fails, it never proves.
;; `qed` re-checks every certificate before it reports a theorem.
;; References:
;; - core.proof (obligation/solve/check, the canonical proposition format)
module core.prover(
   term, canon, show, goal, begin, by, apply_rule, split_conj, intro, cases, subst,
   split_range, decide, auto, qed, state_text, goal_text, open_goals,
   PROVER_VERSION)
use core
use core.str
use core.proof as proof

def PROVER_VERSION = "core.prover@1"

;; ---------------------------------------------------------------- terms

fn _node_int(int v) any {
   mut out = {}
   out = out.set("kind", "int")
   ;; A typed int stored into a dict must be boxed (see core.clamp_cursor).
   out = out.set("value", __box_i64(v))
   out
}
fn _node_name(str n) any {
   mut out = {}
   out = out.set("kind", "name")
   out = out.set("name", n)
   out
}
fn _node_not(any v) any {
   mut out = {}
   out = out.set("kind", "not")
   out = out.set("value", v)
   out
}
fn _node_bin(str op, any l, any r) any {
   mut out = {}
   out = out.set("kind", "bin")
   out = out.set("op", op)
   out = out.set("left", l)
   out = out.set("right", r)
   out
}

;; Canonical text of a node (the format core.proof and the compiler share).
fn canon(any node) str {
   def str kind = node.get("kind")
   if kind == "int" { return "int:" + to_str(node.get("value")) }
   if kind == "name" { return "name:" + node.get("name") }
   if kind == "bool" {
      if node.get("value") { return "bool:true" }
      return "bool:false"
   }
   if kind == "not" { return "unary:!(" + canon(node.get("value")) + ")" }
   if kind == "neg" { return "unary:-(" + canon(node.get("value")) + ")" }
   "binary:" + node.get("op") + "(" + canon(node.get("left")) + "," +
   canon(node.get("right")) + ")"
}

;; Source spelling of a node, fully parenthesised below the top.
fn _show_at(any node, bool top) str {
   def str kind = node.get("kind")
   if kind == "int" { return to_str(node.get("value")) }
   if kind == "name" { return node.get("name") }
   if kind == "bool" {
      if node.get("value") { return "true" }
      return "false"
   }
   if kind == "not" { return "!" + _show_at(node.get("value"), false) }
   if kind == "neg" { return "-" + _show_at(node.get("value"), false) }
   def body = _show_at(node.get("left"), false) + " " + node.get("op") + " " +
   _show_at(node.get("right"), false)
   if top { return body }
   "(" + body + ")"
}

fn show(any node) str { _show_at(node, true) }

;; Tokenizer for `term`: integers, names, and the operators below.
fn _is_space(int b) bool { b == 32 || b == 9 || b == 10 || b == 13 }
fn _is_digit_b(int b) bool { b >= 48 && b <= 57 }
fn _is_name_b(int b) bool {
   (b >= 97 && b <= 122) || (b >= 65 && b <= 90) || b == 95 || _is_digit_b(b)
}

fn _tokens(str text) any {
   mut out = []
   mut i = 0
   def n = len(text)
   while i < n {
      def b = load8(text, i)
      if _is_space(b) {
         i += 1
         continue
      }
      if _is_digit_b(b) {
         mut j = i
         while j < n && _is_digit_b(load8(text, j)) { j += 1 }
         out = out.append(slice(text, i, j))
         i = j
         continue
      }
      if _is_name_b(b) {
         mut j = i
         while j < n && _is_name_b(load8(text, j)) { j += 1 }
         out = out.append(slice(text, i, j))
         i = j
         continue
      }
      def two = slice(text, i, i + 2)
      if two == "&&" || two == "||" || two == "==" || two == "!=" ||
      two == "<=" || two == ">=" || two == "->" {
         out = out.append(two)
         i += 2
         continue
      }
      out = out.append(slice(text, i, i + 1))
      i += 1
   }
   out
}

mut _toks = []
mut _pos = 0
mut _err = ""

fn _peek() str {
   if _pos < len(_toks) { return _toks.get(_pos, "") }
   ""
}

fn _advance() int {
   _pos = _pos + 1
   _pos
}

;; Each parse step returns a node; a failure records `_err`, after which
;; every step returns the placeholder node unchanged.
fn _p_atom() any {
   if _err != "" { return _node_int(0) }
   def str t = _peek()
   if t == "" {
      _err = "unexpected end of term"
      return _node_int(0)
   }
   if t == "(" {
      _advance()
      def inner = _p_imp()
      if _err != "" { return inner }
      if _peek() != ")" {
         _err = "expected ')'"
         return inner
      }
      _advance()
      return inner
   }
   if t == "true" || t == "false" {
      _advance()
      mut node = {}
      node = node.set("kind", "bool")
      node = node.set("value", t == "true")
      return node
   }
   def b = load8(t, 0)
   if _is_digit_b(b) {
      mut v = 0
      mut i = 0
      while i < len(t) {
         v = v * 10 + (load8(t, i) - 48)
         i += 1
      }
      _advance()
      return _node_int(v)
   }
   if _is_name_b(b) {
      _advance()
      return _node_name(t)
   }
   _err = "unexpected '" + t + "'"
   _node_int(0)
}

fn _p_unary() any {
   def str t = _peek()
   if t == "!" {
      _advance()
      return _node_not(_p_unary())
   }
   if t == "-" {
      _advance()
      def inner = _p_unary()
      if inner.get("kind") == "int" { return _node_int(0 - inner.get("value")) }
      mut node = {}
      node = node.set("kind", "neg")
      node = node.set("value", inner)
      return node
   }
   _p_atom()
}

fn _p_level(any ops, int level) any {
   mut left = _p_next(level)
   while _err == "" && contains(ops, _peek()) {
      def str op = _peek()
      _advance()
      def right = _p_next(level)
      left = _node_bin(op, left, right)
   }
   left
}

;; Precedence, loosest first: -> || && comparison + - * / %
fn _p_next(int level) any {
   if level == 0 { return _p_level(["||"], 1) }
   if level == 1 { return _p_level(["&&"], 2) }
   if level == 2 { return _p_level(["==", "!=", "<", "<=", ">", ">="], 3) }
   if level == 3 { return _p_level(["+", "-"], 4) }
   if level == 4 { return _p_level(["*", "/", "%"], 5) }
   _p_unary()
}

;; `a -> b` is `!a || b`, right associative.
fn _p_imp() any {
   def premise = _p_next(0)
   if _err != "" || _peek() != "->" { return premise }
   _advance()
   def rest = _p_imp()
   _node_bin("||", _node_not(premise), rest)
}

;; Reads a proposition from source spelling. Returns the node; a malformed
;; term returns {"kind":"error","reason":s}, which no rule accepts.
fn term(str text) any {
   _toks = _tokens(text)
   _pos = 0
   _err = ""
   def node = _p_imp()
   if _err == "" && _pos != len(_toks) { _err = "trailing input after term" }
   if _err != "" {
      mut bad = {}
      bad = bad.set("kind", "error")
      bad = bad.set("reason", _err)
      return bad
   }
   node
}

;; ---------------------------------------------------------------- goals

;; A goal: hypotheses (source strings or nodes), target, and ranges in the
;; core.proof spelling ("x=0..9" entries joined by the separator below).
fn _as_node(any t) any {
   if is_str(t) { return term(t) }
   t
}

fn goal(any hyps, any target, str ranges="") any {
   mut hs = []
   mut i = 0
   while i < len(hyps) {
      hs = hs.append(_as_node(get(hyps, i)))
      i += 1
   }
   mut g = {}
   g = g.set("hyps", hs)
   g = g.set("target", _as_node(target))
   g = g.set("ranges", ranges)
   g
}

fn _with(any g, any hyps, any target) any {
   goal(hyps, target, g.get("ranges"))
}

fn begin(any g) any {
   mut state = {}
   state = state.set("goals", [g])
   state = state.set("certs", [])
   state = state.set("log", [])
   state = state.set("error", "")
   state
}

fn open_goals(any st) int { len(st.get("goals")) }

fn goal_text(any g) str {
   mut out = ""
   def hs = g.get("hyps")
   mut i = 0
   while i < len(hs) {
      if i > 0 { out = out + ", " }
      out = out + show(get(hs, i))
      i += 1
   }
   out = out + " |- " + show(g.get("target"))
   if g.get("ranges") != "" { out = out + "  [" + g.get("ranges") + "]" }
   out
}

;; The goal state as text, one open goal per line.
fn state_text(any st) str {
   def gs = st.get("goals")
   if len(gs) == 0 { return "no goals" }
   mut out = ""
   mut i = 0
   while i < len(gs) {
      out = out + "goal " + to_str(i + 1) + "/" + to_str(len(gs)) + ": " +
      goal_text(get(gs, i))
      if i + 1 < len(gs) { out = out + "\n" }
      i += 1
   }
   out
}

;; ------------------------------------------------------- rule results

fn _ok(str rule, any goals) any {
   mut r = {}
   r = r.set("ok", true)
   r = r.set("rule", rule)
   r = r.set("goals", goals)
   r = r.set("certs", [])
   r
}

fn _no(str rule, str why) any {
   mut r = {}
   r = r.set("ok", false)
   r = r.set("rule", rule)
   r = r.set("why", why)
   r
}

fn _conj(any nodes) any {
   if len(nodes) == 0 {
      mut t = {}
      t = t.set("kind", "bool")
      t = t.set("value", true)
      return t
   }
   mut acc = get(nodes, 0)
   mut i = 1
   while i < len(nodes) {
      acc = _node_bin("&&", acc, get(nodes, i))
      i += 1
   }
   acc
}

;; The sequent `hyps |- target` as one proposition.
fn _sequent(any g) any {
   def hs = g.get("hyps")
   if len(hs) == 0 { return g.get("target") }
   _node_bin("||", _node_not(_conj(hs)), g.get("target"))
}

;; ---------------------------------------------------- structural rules

;; split_conj:  |- a && b   gives   |- a   and   |- b
fn split_conj(any g) any {
   def t = g.get("target")
   if t.get("kind") != "bin" || t.get("op") != "&&" {
      return _no("split_conj", "target is not a conjunction")
   }
   def hs = g.get("hyps")
   _ok("split_conj", [_with(g, hs, t.get("left")), _with(g, hs, t.get("right"))])
}

;; |- !a || b  (a -> b)   gives   a |- b
fn intro(any g) any {
   def t = g.get("target")
   if t.get("kind") != "bin" || t.get("op") != "||" {
      return _no("intro", "target is not an implication")
   }
   def premise = t.get("left")
   if premise.get("kind") != "not" {
      return _no("intro", "target is not an implication")
   }
   def hs = g.get("hyps")
   _ok("intro", [_with(g, hs.append(premise.get("value")), t.get("right"))])
}

;; a || b, H |- t   gives   a, H |- t   and   b, H |- t
fn cases(any g) any {
   def hs = g.get("hyps")
   mut i = 0
   while i < len(hs) {
      def h = get(hs, i)
      if h.get("kind") == "bin" && h.get("op") == "||" {
         mut rest = []
         mut k = 0
         while k < len(hs) {
            if k != i { rest = rest.append(get(hs, k)) }
            k += 1
         }
         return _ok("cases", [_with(g, rest.append(h.get("left")),
            g.get("target")), _with(g, rest.append(h.get("right")),
            g.get("target"))])
      }
      i += 1
   }
   _no("cases", "no disjunctive hypothesis")
}

fn _mentions(any node, str name) bool {
   def str kind = node.get("kind")
   if kind == "name" { return node.get("name") == name }
   if kind == "not" || kind == "neg" { return _mentions(node.get("value"), name) }
   if kind == "bin" {
      return _mentions(node.get("left"), name) ||
      _mentions(node.get("right"), name)
   }
   false
}

fn _replace(any node, str name, any repl) any {
   def str kind = node.get("kind")
   if kind == "name" {
      if node.get("name") == name { return repl }
      return node
   }
   if kind == "not" { return _node_not(_replace(node.get("value"), name, repl)) }
   if kind == "neg" {
      mut neg = {}
      neg = neg.set("kind", "neg")
      neg = neg.set("value", _replace(node.get("value"), name, repl))
      return neg
   }
   if kind == "bin" {
      return _node_bin(node.get("op"), _replace(node.get("left"), name, repl),
         _replace(node.get("right"), name, repl))
   }
   node
}

fn _drop_range(str ranges, str name) str {
   if ranges == "" { return "" }
   def sep = chr(59)
   def entries = split(ranges, sep)
   mut out = ""
   mut i = 0
   while i < len(entries) {
      def str e = get(entries, i)
      if !startswith(e, name + "=") {
         if out != "" { out = out + sep }
         out = out + e
      }
      i += 1
   }
   out
}

;; x == e, H |- t   gives   H[e/x] |- t[e/x]   (x not in e). Leibniz: any
;; value of x the hypothesis allows equals e. The range of x is dropped,
;; which only strengthens the new goal.
fn subst(any g) any {
   def hs = g.get("hyps")
   mut i = 0
   while i < len(hs) {
      def h = get(hs, i)
      if h.get("kind") == "bin" && h.get("op") == "==" {
         mut name = ""
         mut repl = {}
         def l = h.get("left")
         def r = h.get("right")
         if l.get("kind") == "name" && !_mentions(r, l.get("name")) {
            name = l.get("name")
            repl = r
         } elif r.get("kind") == "name" && !_mentions(l, r.get("name")) {
            name = r.get("name")
            repl = l
         }
         if name != "" {
            mut rest = []
            mut k = 0
            while k < len(hs) {
               if k != i { rest = rest.append(_replace(get(hs, k), name, repl)) }
               k += 1
            }
            return _ok("subst " + name,
               [goal(rest, _replace(g.get("target"), name, repl),
                  _drop_range(g.get("ranges"), name))])
         }
      }
      i += 1
   }
   _no("subst", "no hypothesis of the form name == term")
}

;; x in lo..hi   gives   x in lo..mid   and   x in mid+1..hi
fn split_range(any g, str name) any {
   def str ranges = g.get("ranges")
   def sep = chr(59)
   def entries = split(ranges, sep)
   mut i = 0
   while i < len(entries) {
      def str e = get(entries, i)
      if startswith(e, name + "=") {
         def body = slice(e, len(name) + 1, len(e))
         def ends = split(body, "..")
         if len(ends) != 2 { return _no("split_range", "malformed range") }
         def lo = _to_int(get(ends, 0))
         def hi = _to_int(get(ends, 1))
         if lo >= hi { return _no("split_range", "range has one value") }
         def mid = lo + (hi - lo) / 2
         def a = name + "=" + to_str(lo) + ".." + to_str(mid)
         def b = name + "=" + to_str(mid + 1) + ".." + to_str(hi)
         return _ok("split_range " + name,
            [goal(g.get("hyps"), g.get("target"), _swap_range(entries, i, a)),
               goal(g.get("hyps"), g.get("target"), _swap_range(entries, i, b))])
      }
      i += 1
   }
   _no("split_range", "no range for " + name)
}

fn _to_int(str st) int {
   mut v = 0
   mut sign = 1
   mut i = 0
   if len(st) > 0 && load8(st, 0) == 45 {
      sign = -1
      i = 1
   }
   while i < len(st) {
      v = v * 10 + (load8(st, i) - 48)
      i += 1
   }
   v * sign
}

fn _swap_range(any entries, int at, str entry) str {
   def sep = chr(59)
   mut out = ""
   mut i = 0
   while i < len(entries) {
      if i > 0 { out = out + sep }
      if i == at { out = out + entry } else { out = out + get(entries, i) }
      i += 1
   }
   out
}

;; ---------------------------------------------------------- closing

;; Closes a goal repl a checked certificate for `hyps -> target`.
fn decide(any g) any {
   def ob = proof.obligation(canon(_sequent(g)), g.get("ranges"))
   def cert = proof.solve(ob)
   if proof.verdict_of(cert) != "proved" {
      return _no("decide", "core.proof: " + proof.verdict_of(cert))
   }
   if !proof.check(cert) {
      return _no("decide", "the checker rejected the certificate")
   }
   mut r = _ok("decide", [])
   r = r.set("certs", [cert])
   r
}

;; ------------------------------------------------------------ driving

fn _replace_first(any st, any r) any {
   def gs = st.get("goals")
   mut next = r.get("goals")
   mut i = 1
   while i < len(gs) {
      next = next.append(get(gs, i))
      i += 1
   }
   mut certs = st.get("certs")
   def new_certs = r.get("certs")
   mut k = 0
   while k < len(new_certs) {
      certs = certs.append(get(new_certs, k))
      k += 1
   }
   mut out = st.set("goals", next)
   out = out.set("certs", certs)
   out = out.set("log", st.get("log").append(r.get("rule")))
   out
}

;; Applies a built-in rule (one of the functions above) to the first goal.
fn apply_rule(any st, any rule) any {
   def gs = st.get("goals")
   if len(gs) == 0 { return st.set("error", "no goals") }
   def r = rule(get(gs, 0))
   if !r.get("ok") { return st.set("error", r.get("rule") + ": " + r.get("why")) }
   _replace_first(st, r)
}

;; Runs an untrusted user tactic on the first goal. Its subgoals replace the
;; goal only if the checker proves they entail it under the same ranges:
;; (s1 && ... && sn) -> goal. A tactic cannot close a goal by itself.
fn by(any st, any tactic, str name="user") any {
   def gs = st.get("goals")
   if len(gs) == 0 { return st.set("error", "no goals") }
   def g = get(gs, 0)
   def r = tactic(g)
   if !r.get("ok", false) {
      return st.set("error", name + ": " + r.get("why", "tactic failed"))
   }
   def subs = r.get("goals", [])
   if len(subs) == 0 {
      return st.set("error", name + ": a tactic must leave goals, use decide")
   }
   mut seqs = []
   mut i = 0
   while i < len(subs) {
      def sg = get(subs, i)
      if sg.get("ranges") != g.get("ranges") {
         return st.set("error", name + ": a tactic may not change ranges")
      }
      seqs = seqs.append(_sequent(sg))
      i += 1
   }
   def entail = _node_bin("||", _node_not(_conj(seqs)), _sequent(g))
   def ob = proof.obligation(canon(entail), g.get("ranges"))
   def cert = proof.solve(ob)
   if proof.verdict_of(cert) != "proved" || !proof.check(cert) {
      return st.set("error", name + ": its goals do not entail the goal")
   }
   mut checked = {}
   checked = checked.set("ok", true)
   checked = checked.set("rule", name)
   checked = checked.set("goals", subs)
   checked = checked.set("certs", [cert])
   _replace_first(st, checked)
}

fn _first_range_name(str ranges) str {
   if ranges == "" { return "" }
   def entries = split(ranges, chr(59))
   mut best = ""
   mut width = 0
   mut i = 0
   while i < len(entries) {
      def str e = get(entries, i)
      def at = find_from(e, "=", 0)
      if at > 0 {
         def ends = split(slice(e, at + 1, len(e)), "..")
         if len(ends) == 2 {
            def w = _to_int(get(ends, 1)) - _to_int(get(ends, 0))
            if w > width {
               width = w
               best = slice(e, 0, at)
            }
         }
      }
      i += 1
   }
   best
}

;; Search: decide, else the first structural rule that applies, else split
;; the widest range; `depth` bounds the tree. Every leaf still closes only
;; through `decide`.
fn auto(any state, int depth=8) any {
   mut st = state
   mut budget = 256
   while open_goals(st) > 0 && budget > 0 {
      budget -= 1
      def g = get(st.get("goals"), 0)
      def closed = apply_rule(st, decide)
      if closed.get("error") == "" {
         st = closed
         continue
      }
      mut moved = false
      for rule in [split_conj, intro, cases, subst] {
         if !moved {
            def next = apply_rule(st, rule)
            if next.get("error") == "" {
               st = next
               moved = true
            }
         }
      }
      if moved { continue }
      def name = _first_range_name(g.get("ranges"))
      if name == "" || len(st.get("log")) > depth * 32 {
         return st.set("error", "auto: stuck on " + goal_text(g))
      }
      def r = split_range(g, name)
      if !r.get("ok") { return st.set("error", "auto: " + r.get("why")) }
      st = _replace_first(st, r)
   }
   if open_goals(st) > 0 { return st.set("error", "auto: budget exhausted") }
   st
}

;; A theorem: no open goals and every certificate re-checked.
fn qed(any st) any {
   mut out = {}
   out = out.set("steps", st.get("log"))
   out = out.set("certificates", len(st.get("certs")))
   if st.get("error") != "" {
      out = out.set("proved", false)
      return out.set("why", st.get("error"))
   }
   if open_goals(st) > 0 {
      out = out.set("proved", false)
      return out.set("why", to_str(open_goals(st)) + " open goal(st)")
   }
   def certs = st.get("certs")
   mut i = 0
   while i < len(certs) {
      if !proof.check(get(certs, i)) {
         out = out.set("proved", false)
         return out.set("why", "certificate " + to_str(i) + " failed recheck")
      }
      i += 1
   }
   out = out.set("proved", true)
   out.set("why", "")
}
