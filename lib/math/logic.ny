;; Keywords: logic boolean proposition proof reasoning math
;; Self-hosted propositional reasoning for Nytrix.
;; References:
;; - core.math
module core.math.logic(any, all,
   truth, falsity, atom, neg, conj, disj, implies, iff,
   evaluate, simplify, variables, decide, valid, canonical, digest,
   certificate, check_certificate,
   prop_true, prop_false, prop_atom, prop_not, prop_and, prop_or,
   prop_implies, prop_iff, prop_is, prop_eval, prop_simplify,
   prop_variables, prop_tautology_report, prop_tautology,
   prop_canonical, prop_digest, prop_certificate, prop_check_certificate)
use core.math
use core
use core.reflect
use core.math.logic.certificate as cert

def LOGIC_MODULE_VERSION = "core.math.logic@1"
def LOGIC_DEPENDENCY_DIGEST = "core+core.math@1"

fn any(any xs) bool {
   "Returns true if at least one element in `xs` is truthy. If `xs` is not a list, returns `bool(xs)`."
   if !is_list(xs) { return bool(xs) }
   mut i = 0
   while i < xs.len {
      if bool(xs.get(i)) { return true }
      i += 1
   }
   false
}

fn all(any xs) bool {
   "Returns true if all elements in `xs` are truthy. If `xs` is not a list, returns `bool(xs)`."
   if !is_list(xs) { return bool(xs) }
   mut i = 0
   while i < xs.len {
      if !bool(xs.get(i)) { return false }
      i += 1
   }
   true
}

;; Creates the true proposition.
fn prop_true() dict { {"kind": "true"} }

;; Creates the false proposition.
fn prop_false() dict { {"kind": "false"} }

;; Creates a named atomic proposition.
fn prop_atom(str name) dict {
   assert(name.len > 0, "prop_atom expects a non-empty name")
   return {"kind": "atom", "name": name}
}

;; Creates the negation of a proposition.
fn prop_not(dict value) dict {
   assert(prop_is(value), "prop_not expects a proposition")
   return {"kind": "not", "value": value}
}

;; Creates the conjunction of two propositions.
fn prop_and(dict left, dict right) dict {
   assert(prop_is(left) && prop_is(right), "prop_and expects propositions")
   return {"kind": "and", "left": left, "right": right}
}

;; Creates the disjunction of two propositions.
fn prop_or(dict left, dict right) dict {
   assert(prop_is(left) && prop_is(right), "prop_or expects propositions")
   return {"kind": "or", "left": left, "right": right}
}

;; Creates an implication between two propositions.
fn prop_implies(dict premise, dict conclusion) dict {
   assert(prop_is(premise) && prop_is(conclusion), "prop_implies expects propositions")
   return {"kind": "implies", "left": premise, "right": conclusion}
}

;; Creates a biconditional between two propositions.
fn prop_iff(dict left, dict right) dict {
   assert(prop_is(left) && prop_is(right), "prop_iff expects propositions")
   return {"kind": "iff", "left": left, "right": right}
}

;; Returns true when the value is a well-formed proposition.
fn prop_is(any value) bool {
   if !is_dict(value) { return false }
   def kind = value.get("kind", "")
   if kind == "true" || kind == "false" { return true }
   if kind == "atom" { return is_str(value.get("name", 0)) && value.get("name", "").len > 0 }
   if kind == "not" { return prop_is(value.get("value", 0)) }
   if kind == "and" || kind == "or" || kind == "implies" || kind == "iff" {
      return prop_is(value.get("left", 0)) && prop_is(value.get("right", 0))
   }
   false
}

;; Evaluates a proposition in the supplied environment.
fn prop_eval(dict proposition, dict environment={}) bool {
   assert(prop_is(proposition), "prop_eval expects a proposition")
   def kind = proposition.get("kind")
   if kind == "true" { return true }
   if kind == "false" { return false }
   if kind == "atom" { return bool(environment.get(proposition.get("name"), false)) }
   if kind == "not" { return !prop_eval(proposition.get("value"), environment) }
   def left = prop_eval(proposition.get("left"), environment)
   def right = prop_eval(proposition.get("right"), environment)
   if kind == "and" { return left && right }
   if kind == "or" { return left || right }
   if kind == "implies" { return !left || right }
   left == right
}

;; Returns a simplified equivalent proposition.
fn prop_simplify(dict proposition) dict {
   assert(prop_is(proposition), "prop_simplify expects a proposition")
   def kind = proposition.get("kind")
   if kind == "true" || kind == "false" || kind == "atom" { return proposition }
   if kind == "not" {
      def value = prop_simplify(proposition.get("value"))
      def inner = value.get("kind")
      if inner == "true" { return prop_false() }
      if inner == "false" { return prop_true() }
      if inner == "not" { return prop_simplify(value.get("value")) }
      return prop_not(value)
   }
   def left = prop_simplify(proposition.get("left"))
   def right = prop_simplify(proposition.get("right"))
   def lk, rk = left.get("kind"), right.get("kind")
   if kind == "and" {
      if lk == "false" || rk == "false" { return prop_false() }
      if lk == "true" { return right }
      if rk == "true" { return left }
      return prop_and(left, right)
   }
   if kind == "or" {
      if lk == "true" || rk == "true" { return prop_true() }
      if lk == "false" { return right }
      if rk == "false" { return left }
      return prop_or(left, right)
   }
   if kind == "implies" {
      if lk == "false" || rk == "true" { return prop_true() }
      if lk == "true" { return right }
      if rk == "false" { return prop_not(left) }
      return prop_implies(left, right)
   }
   if lk == "true" { return right }
   if rk == "true" { return left }
   if lk == "false" { return prop_not(right) }
   if rk == "false" { return prop_not(left) }
   return prop_iff(left, right)
}

fn _prop_names_add(list names, str name) list {
   mut i = 0
   while i < names.len {
      if __str_eq(names.get(i), name) { return names }
      i += 1
   }
   names = names.append(name)
   names
}

fn _prop_variables_into(dict proposition, list names) list {
   def kind = proposition.get("kind")
   if kind == "atom" { return _prop_names_add(names, proposition.get("name")) }
   if kind == "not" { return _prop_variables_into(proposition.get("value"), names) }
   if kind == "and" || kind == "or" || kind == "implies" || kind == "iff" {
      names = _prop_variables_into(proposition.get("left"), names)
      return _prop_variables_into(proposition.get("right"), names)
   }
   names
}

;; Returns the atom names used by a proposition.
fn prop_variables(dict proposition) list {
   assert(prop_is(proposition), "prop_variables expects a proposition")
   _prop_variables_into(proposition, [])
}

@noinline
fn _prop_measure(dict proposition, int depth, int max_depth,
   int nodes, int max_nodes, int memory, int max_memory) dict {
   if depth > max_depth {
      return {"decided": false, "reason": "depth limit",
         "nodes": nodes, "memory": memory}
   }
   if nodes >= max_nodes {
      return {"decided": false, "reason": "node limit",
         "nodes": nodes, "memory": memory}
   }
   if memory >= max_memory {
      return {"decided": false, "reason": "memory limit",
         "nodes": nodes, "memory": memory}
   }
   nodes += 1
   memory += 1
   def kind = proposition.get("kind")
   if kind == "not" {
      return _prop_measure(proposition.get("value"), depth + 1,
         max_depth, nodes, max_nodes, memory, max_memory)
   }
   if kind == "and" || kind == "or" || kind == "implies" || kind == "iff" {
      def left = _prop_measure(proposition.get("left"), depth + 1,
         max_depth, nodes, max_nodes, memory, max_memory)
      if !left.get("decided") { return left }
      return _prop_measure(proposition.get("right"), depth + 1,
         max_depth, left.get("nodes"), max_nodes,
         left.get("memory"), max_memory)
   }
   {"decided": true, "reason": "complete", "nodes": nodes,
      "memory": memory}
}

@noinline
fn _prop_eval_bounded(dict proposition, dict environment,
   int depth, int max_depth, int steps, int max_steps) dict {
   if depth > max_depth {
      return {"decided": false, "value": false,
         "steps": steps, "reason": "depth limit"}
   }
   if steps >= max_steps {
      return {"decided": false, "value": false,
         "steps": steps, "reason": "step limit"}
   }
   steps += 1
   def str kind = proposition.get("kind")
   if kind == "true" { return {"decided": true, "value": true,
         "steps": steps, "reason": "complete"} }
   if kind == "false" { return {"decided": true, "value": false,
         "steps": steps, "reason": "complete"} }
   if kind == "atom" { return {"decided": true,
         "value": bool(environment.get(proposition.get("name"), false)),
         "steps": steps, "reason": "complete"} }
   if kind == "not" {
      def result = _prop_eval_bounded(proposition.get("value"), environment,
         depth + 1, max_depth, steps, max_steps)
      if !result.get("decided") { return result }
      return {"decided": true, "value": !result.get("value"),
         "steps": result.get("steps"), "reason": "complete"}
   }
   def left_result = _prop_eval_bounded(proposition.get("left"), environment,
      depth + 1, max_depth, steps, max_steps)
   if !left_result.get("decided") { return left_result }
   def right_result = _prop_eval_bounded(proposition.get("right"), environment,
      depth + 1, max_depth, left_result.get("steps"), max_steps)
   if !right_result.get("decided") { return right_result }
   def left = left_result.get("value")
   def right = right_result.get("value")
   if kind == "and" { return {"decided": true, "value": left && right,
         "steps": right_result.get("steps"), "reason": "complete"} }
   if kind == "or" { return {"decided": true, "value": left || right,
         "steps": right_result.get("steps"), "reason": "complete"} }
   if kind == "implies" { return {"decided": true, "value": !left || right,
         "steps": right_result.get("steps"), "reason": "complete"} }
   {"decided": true, "value": left == right,
      "steps": right_result.get("steps"), "reason": "complete"}
}

;; Decides a bounded proposition and returns its proof report.
fn _prop_tautology_report_impl(dict proposition, int max_variables,
   int max_steps, int max_depth, int max_nodes, int max_memory) dict {
   assert(prop_is(proposition), "prop_tautology_report expects a proposition")
   if max_variables < 0 || max_variables > 20 || max_steps <= 0 ||
   max_depth <= 0 || max_nodes <= 0 || max_memory <= 0 {
      return {"decided":false, "valid":false, "variables":[],
         "reason":"invalid budget", "counterexample":{},
         "assignments_checked":0, "assignments_required":-1,
         "steps":0, "nodes":0, "memory":0}
   }
   ;; Reject the zero-variable budget before recursive resource accounting.
   ;; This keeps the diagnostic deterministic even when the evaluator has
   ;; already been specialized for an earlier call in the same module.
   if max_variables == 0 {
      def early_names = prop_variables(proposition)
      return {"decided": false, "valid": false, "variables": early_names,
         "reason": "variable limit", "counterexample": {},
         "assignments_checked":0,
         "assignments_required":early_names.len <= 20 ? 1 << early_names.len : -1,
         "steps":0, "nodes":0, "memory":0}
   }
   mut state = {"decided":true, "reason":"complete", "steps":0,
      "nodes":0, "memory":0}
   ;; Reinitialize every mutable field explicitly.  State dictionaries may be
   ;; materialized through pooled literal storage by optimized native code;
   ;; relying only on literal construction made a later budget call inherit
   ;; the previous evaluator's depth/decision state.
   state.set("decided", true)
   state.set("reason", "complete")
   state.set("steps", 0)
   state.set("nodes", 0)
   state.set("memory", 0)
   def measure = _prop_measure(proposition, 0, max_depth, 0,
      max_nodes, 0, max_memory)
   state.set("nodes", measure.get("nodes"))
   state.set("memory", measure.get("memory"))
   if !measure.get("decided") {
      return {"decided":false, "valid":false, "variables":[],
         "reason":measure.get("reason"), "counterexample":{},
         "assignments_checked":0, "assignments_required":-1,
         "steps":state.get("steps"), "nodes":state.get("nodes"),
         "memory":state.get("memory")}
   }
   def names = prop_variables(proposition)
   if names.len > max_variables {
      return {"decided": false, "valid": false, "variables": names,
         "reason": "variable limit", "counterexample": {},
         "assignments_checked":0,
         "assignments_required":names.len <= 20 ? 1 << names.len : -1,
         "steps":state.get("steps"), "nodes":state.get("nodes"),
         "memory":state.get("memory")}
   }
   if state.get("memory") + names.len * 2 > max_memory {
      return {"decided":false, "valid":false, "variables":names,
         "reason":"memory limit", "counterexample":{},
         "assignments_checked":0, "assignments_required":1 << names.len,
         "steps":state.get("steps"), "nodes":state.get("nodes"),
         "memory":state.get("memory")}
   }
   def assignments = 1 << names.len
   mut mask = 0
   while mask < assignments {
      mut environment = {}
      mut i = 0
      while i < names.len {
         environment = environment.set(names.get(i), ((mask >> i) & 1) == 1)
         i += 1
      }
      def evaluation = _prop_eval_bounded(proposition, environment, 0,
         max_depth, state.get("steps"), max_steps)
      state.set("steps", evaluation.get("steps"))
      if !evaluation.get("value") {
         if !evaluation.get("decided") {
            return {"decided":false, "valid":false, "variables":names,
               "reason":evaluation.get("reason"), "counterexample":{},
               "assignments_checked":mask,
               "assignments_required":assignments,
               "steps":state.get("steps"), "nodes":state.get("nodes"),
               "memory":state.get("memory")}
         }
         return {"decided": true, "valid": false, "variables": names,
            "reason": "counterexample", "counterexample": environment,
            "assignments_checked":mask + 1,
            "assignments_required":assignments,
            "steps":state.get("steps"), "nodes":state.get("nodes"),
            "memory":state.get("memory")}
      }
      mask += 1
   }
   return {"decided": true, "valid": true, "variables": names,
      "reason": "exhaustive", "counterexample": {},
      "assignments_checked":assignments,
      "assignments_required":assignments,
      "steps":state.get("steps"), "nodes":state.get("nodes"),
      "memory":state.get("memory")}
}

;; Keep default-argument expansion in a small ABI-stable wrapper.  The
;; evaluator itself has six required scalar parameters, so repeated calls
;; with different omitted-tail defaults cannot reuse a partially normalized
;; call frame.
fn prop_tautology_report(dict proposition, int max_variables=16,
   int max_steps=1000000, int max_depth=128, int max_nodes=100000,
   int max_memory=100000) dict {
   _prop_tautology_report_impl(proposition, max_variables, max_steps,
      max_depth, max_nodes, max_memory)
}

;; Returns true when bounded evaluation proves the proposition valid.
fn prop_tautology(dict proposition, int max_variables=16,
   int max_steps=1000000, int max_depth=128, int max_nodes=100000,
   int max_memory=100000) bool {
   def report = prop_tautology_report(proposition, max_variables, max_steps,
      max_depth, max_nodes, max_memory)
   report.get("decided") && report.get("valid")
}

;; Returns the canonical encoding of a proposition.
fn prop_canonical(dict proposition) str {
   assert(prop_is(proposition), "prop_canonical expects a proposition")
   def kind = proposition.get("kind")
   if kind == "true" { return "T" }
   if kind == "false" { return "F" }
   if kind == "atom" {
      def name = proposition.get("name")
      return "A" + to_str(name.len) + ":" + name
   }
   if kind == "not" {
      def value = prop_canonical(proposition.get("value"))
      return "N" + to_str(value.len) + ":" + value
   }
   def left = prop_canonical(proposition.get("left"))
   def right = prop_canonical(proposition.get("right"))
   mut tag = "&"
   if kind == "or" { tag = "|" }
   elif kind == "implies" { tag = ">" }
   elif kind == "iff" { tag = "=" }
   tag + to_str(left.len) + ":" + left + to_str(right.len) + ":" + right
}

;; Returns the stable digest of a proposition.
fn prop_digest(dict proposition) str {
   def encoded = prop_canonical(proposition)
   "logic-prop-v1:" + to_str(hash(encoded)) + ":" + to_str(encoded.len)
}

;; Builds a bounded decision certificate for a proposition.
fn prop_certificate(dict proposition, int max_variables=16,
   int max_steps=1000000, int max_depth=128, int max_nodes=100000,
   int max_memory=100000) dict {
   def encoded = prop_canonical(proposition)
   def decision = prop_tautology_report(proposition, max_variables, max_steps,
      max_depth, max_nodes, max_memory)
   def base = cert.envelope(encoded, LOGIC_MODULE_VERSION,
      LOGIC_DEPENDENCY_DIGEST)
   return base.merge({"version":"logic-cert-v1", "checker":"truth-table-v1",
         "proposition":proposition, "canonical":encoded,
         "digest":prop_digest(proposition), "max_variables":max_variables,
         "max_steps":max_steps, "max_depth":max_depth,
         "max_nodes":max_nodes, "max_memory":max_memory,
         "decision":decision, "envelope_digest":base.get("digest")})
}

;; Checks the structure and decision recorded in a proposition certificate.
fn prop_check_certificate(any value) bool {
   if !is_dict(value) || value.get("version", "") != "logic-cert-v1" ||
   value.get("checker", "") != "truth-table-v1" ||
   !prop_is(value.get("proposition", 0)) ||
   !is_str(value.get("canonical", 0)) || !is_str(value.get("digest", 0)) ||
   !is_int(value.get("envelope_digest", nil)) ||
   !is_int(value.get("max_variables", nil)) ||
   !is_int(value.get("max_steps", nil)) ||
   !is_int(value.get("max_depth", nil)) ||
   !is_int(value.get("max_nodes", nil)) ||
   !is_int(value.get("max_memory", nil)) ||
   !is_dict(value.get("decision", 0)) {
      return false
   }
   def proposition = value.get("proposition")
   def encoded = prop_canonical(proposition)
   def envelope = {"format":value.get("format", ""), "canonical":encoded,
      "digest":value.get("envelope_digest"),
      "module_version":value.get("module_version", ""),
      "dependency_digest":value.get("dependency_digest", ""),
      "checker_version":value.get("checker_version", "")}
   if encoded != value.get("canonical") ||
   prop_digest(proposition) != value.get("digest") ||
   !cert.check(envelope, value.get("max_variables"), value.get("max_nodes"),
      value.get("max_depth"), value.get("max_steps"),
      value.get("max_memory")) {
      return false
   }
   def checked = prop_tautology_report(proposition, value.get("max_variables"),
      value.get("max_steps"), value.get("max_depth"),
      value.get("max_nodes"), value.get("max_memory"))
   def claimed = value.get("decision")
   ;; Dynamic dictionary booleans are Ny immediates (2/8); the typed bool
   ;; return below keeps its predicate representation at the function ABI.
   def final_ok = checked.get("decided") && checked.get("valid") &&
   claimed.get("decided", false) && claimed.get("valid", false)
   if final_ok {
      return true
   }
   return false
}

;; Compact public vocabulary. The prop_* names remain available when explicit
;; representation-oriented code is clearer.
fn truth() dict { prop_true() }

fn falsity() dict { prop_false() }

fn atom(str name) dict { prop_atom(name) }

fn neg(dict proposition) dict { prop_not(proposition) }

fn conj(dict left, dict right) dict { prop_and(left, right) }

fn disj(dict left, dict right) dict { prop_or(left, right) }

fn implies(dict premise, dict conclusion) dict { prop_implies(premise, conclusion) }

fn iff(dict left, dict right) dict { prop_iff(left, right) }

;; Returns the result of the `evaluate` operation.
fn evaluate(dict proposition, dict environment={}) bool { prop_eval(proposition, environment) }

fn simplify(dict proposition) dict { prop_simplify(proposition) }

fn variables(dict proposition) list { prop_variables(proposition) }

;; Returns the result of the `decide` operation.
fn decide(dict proposition, int max_variables=16, int max_steps=1000000,
   int max_depth=128, int max_nodes=100000, int max_memory=100000) dict {
   prop_tautology_report(proposition, max_variables, max_steps, max_depth,
      max_nodes, max_memory)
}

;; Returns the result of the `valid` operation.
fn valid(dict proposition, int max_variables=16, int max_steps=1000000,
   int max_depth=128, int max_nodes=100000, int max_memory=100000) bool {
   prop_tautology(proposition, max_variables, max_steps, max_depth, max_nodes,
      max_memory)
}

fn canonical(dict proposition) str { prop_canonical(proposition) }

fn digest(dict proposition) str { prop_digest(proposition) }

;; Returns the result of the `certificate` operation.
fn certificate(dict proposition, int max_variables=16,
   int max_steps=1000000, int max_depth=128, int max_nodes=100000,
   int max_memory=100000) dict {
   prop_certificate(proposition, max_variables, max_steps, max_depth,
      max_nodes, max_memory)
}

fn check_certificate(any value) bool {
   def result = prop_check_certificate(value)
   if result { return true }
   return false
}
