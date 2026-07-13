;; Keywords: logic congruence arithmetic linear finite induction certificate budget
;; Bounded self-hosted decision procedures with replayable reports.
module std.math.logic.solve(
   congruence, arithmetic, linear, finite, induction)

use std.core
use std.core.reflect as reflect
use std.math.logic.prolog as prolog
use std.math.logic.certificate as cert

def SOLVER_VERSION = "std.math.logic.solve@1"

fn _certificate(str method, any claim, any evidence) dict {
   def dependency = to_str(hash(reflect.repr([method, claim, evidence])))
   cert.envelope("T", SOLVER_VERSION, dependency).merge({
      "method":method, "claim":claim, "evidence":evidence})
}

fn _term_key(any value) str {
   if !prolog.is_term(value) { return type(value) + ":" + to_str(value) }
   mut out = value.get("name") + "("
   mut i = 0
   while i < value.get("args").len {
      def child = _term_key(value.get("args")[i])
      out += to_str(child.len) + ":" + child
      i += 1
   }
   out + ")"
}

fn _consume(dict state, int depth) bool {
   if depth > state.get("max_depth") {
      state["decided"] = false
      state["reason"] = "depth limit"
      return false
   }
   if state.get("nodes") >= state.get("max_nodes") {
      state["decided"] = false
      state["reason"] = "node limit"
      return false
   }
   if state.get("steps") >= state.get("max_steps") {
      state["decided"] = false
      state["reason"] = "step limit"
      return false
   }
   if state.get("memory", 0) >= state.get("max_memory", 1000000) {
      state["decided"] = false
      state["reason"] = "memory limit"
      return false
   }
   state["nodes"] = state.get("nodes") + 1
   state["steps"] = state.get("steps") + 1
   state["memory"] = state.get("memory", 0) + 1
   true
}

fn _variables(any value, dict seen, int depth, int max_depth,
   int max_variables) bool {
   if depth > max_depth { return false }
   if prolog.is_variable(value) {
      seen[value.get("name")] = true
      return seen.len <= max_variables
   }
   if !prolog.is_term(value) { return true }
   mut i = 0
   while i < value.get("args").len {
      if !_variables(value.get("args")[i], seen, depth + 1, max_depth,
         max_variables) { return false }
      i += 1
   }
   true
}

fn _collect_terms(any value, dict terms, dict parent, dict state,
   int depth) bool {
   if !_consume(state, depth) { return false }
   def key = _term_key(value)
   if !terms.contains(key) {
      terms[key] = value
      parent[key] = key
   }
   if !prolog.is_term(value) { return true }
   def args = value.get("args")
   mut i = 0
   while i < args.len {
      if !_collect_terms(args[i], terms, parent, state, depth + 1) {
         return false
      }
      i += 1
   }
   true
}

fn _root(dict parent, str key, dict state) str {
   mut current = key
   mut depth = 0
   while parent.get(current, current) != current {
      if !_consume(state, depth) { return current }
      current = parent.get(current)
      depth += 1
   }
   current
}

fn _union(dict parent, str left, str right, dict state) bool {
   def a = _root(parent, left, state)
   def b = _root(parent, right, state)
   if !state.get("decided") { return false }
   if a == b { return false }
   parent[b] = a
   true
}

fn _congruent(any left, any right, dict parent, dict state) bool {
   if !prolog.is_term(left) || !prolog.is_term(right) ||
      left.get("name") != right.get("name") ||
      left.get("args").len != right.get("args").len {
      return false
   }
   mut i = 0
   while i < left.get("args").len {
      def a = _root(parent, _term_key(left.get("args")[i]), state)
      def b = _root(parent, _term_key(right.get("args")[i]), state)
      if !state.get("decided") || a != b { return false }
      i += 1
   }
   true
}

;; Returns the result of the `congruence` operation.
fn congruence(list equalities, any left, any right, int max_steps=10000,
   int max_nodes=10000, int max_depth=128, int max_variables=256,
   int max_memory=100000) dict {
   if max_steps <= 0 || max_nodes <= 0 || max_depth <= 0 ||
      max_variables <= 0 || max_memory <= 0 {
      return {"decided":false, "reason":"invalid budget", "equal":false}
   }
   mut state = {"decided":true, "reason":"complete", "steps":0,
      "nodes":0, "max_steps":max_steps, "max_nodes":max_nodes,
      "max_depth":max_depth, "memory":0, "max_memory":max_memory}
   mut seen = dict(16)
   if !_variables(left, seen, 0, max_depth, max_variables) ||
      !_variables(right, seen, 0, max_depth, max_variables) {
      return {"decided":false, "reason":"variable limit", "equal":false}
   }
   mut terms = dict(32)
   mut parent = dict(32)
   _collect_terms(left, terms, parent, state, 0)
   _collect_terms(right, terms, parent, state, 0)
   mut i = 0
   while i < equalities.len && state.get("decided") {
      def pair = equalities[i]
      if !is_list(pair) || pair.len != 2 {
         return {"decided":false, "reason":"invalid equality",
            "equal":false, "steps":state.get("steps"),
            "nodes":state.get("nodes")}
      }
      if !_variables(pair[0], seen, 0, max_depth, max_variables) ||
         !_variables(pair[1], seen, 0, max_depth, max_variables) {
         return {"decided":false, "reason":"variable limit", "equal":false}
      }
      _collect_terms(pair[0], terms, parent, state, 0)
      _collect_terms(pair[1], terms, parent, state, 0)
      if state.get("decided") {
         _union(parent, _term_key(pair[0]), _term_key(pair[1]), state)
      }
      i += 1
   }
   def keys = terms.keys()
   mut changed = true
   mut passes = 0
   while changed && state.get("decided") {
      changed = false
      i = 0
      while i < keys.len && state.get("decided") {
         mut j = i + 1
         while j < keys.len && state.get("decided") {
            if !_consume(state, 0) { break }
            if _congruent(terms.get(keys[i]), terms.get(keys[j]), parent, state) {
               changed = _union(parent, keys[i], keys[j], state) || changed
            }
            j += 1
         }
         i += 1
      }
      passes += 1
   }
   def left_key = _term_key(left)
   def right_key = _term_key(right)
   def left_root = _root(parent, left_key, state)
   def right_root = _root(parent, right_key, state)
   def equal = state.get("decided") && left_root == right_root
   def evidence = {"equalities":equalities, "left":left, "right":right,
      "passes":passes}
   return {"decided":state.get("decided"), "reason":state.get("reason"),
      "equal":equal, "steps":state.get("steps"),
      "nodes":state.get("nodes"), "left_root":left_root,
      "right_root":right_root, "variables":seen.len,
      "memory":state.get("memory"),
      "certificate":_certificate("congruence", equal, evidence)}
}

fn _arithmetic_eval(any expression, dict state, int depth) any {
   if !_consume(state, depth) { return nil }
   if is_int(expression) || is_float(expression) { return expression }
   if !prolog.is_term(expression) { return nil }
   def name = expression.get("name")
   def args = expression.get("args")
   if name == "neg" && args.len == 1 {
      def value = _arithmetic_eval(args[0], state, depth + 1)
      return value == nil ? nil : -value
   }
   if args.len != 2 { return nil }
   def left = _arithmetic_eval(args[0], state, depth + 1)
   def right = _arithmetic_eval(args[1], state, depth + 1)
   if left == nil || right == nil { return nil }
   if name == "add" { return left + right }
   if name == "sub" { return left - right }
   if name == "mul" { return left * right }
   if name == "div" && right != 0 { return left / right }
   if name == "mod" && right != 0 { return left % right }
   nil
}

;; Returns the result of the `arithmetic` operation.
fn arithmetic(any expression, int max_steps=10000, int max_nodes=10000,
   int max_depth=128, int max_variables=256, int max_memory=100000) dict {
   if max_steps <= 0 || max_nodes <= 0 || max_depth <= 0 ||
      max_variables <= 0 || max_memory <= 0 {
      return {"decided":false, "reason":"invalid budget", "value":nil}
   }
   mut state = {"decided":true, "reason":"normal form", "steps":0,
      "nodes":0, "max_steps":max_steps, "max_nodes":max_nodes,
      "max_depth":max_depth, "memory":0, "max_memory":max_memory}
   mut seen = dict(16)
   if !_variables(expression, seen, 0, max_depth, max_variables) {
      return {"decided":false, "reason":"variable limit", "value":nil}
   }
   def value = _arithmetic_eval(expression, state, 0)
   if value == nil && state.get("decided") {
      state["decided"] = false
      state["reason"] = "unsupported expression"
   }
   def evidence = {"expression":expression, "value":value}
   return {"decided":state.get("decided"), "reason":state.get("reason"),
      "value":value, "steps":state.get("steps"),
      "nodes":state.get("nodes"), "variables":seen.len,
      "memory":state.get("memory"),
      "certificate":_certificate("arithmetic", value, evidence)}
}

fn _linear_holds(dict constraint, dict assignment, dict state) bool {
   if !_consume(state, 0) { return false }
   def coefficients = constraint.get("coefficients", {})
   mut total = 0
   def names = coefficients.keys()
   mut i = 0
   while i < names.len {
      total += coefficients.get(names[i]) * assignment.get(names[i], 0)
      i += 1
   }
   def rhs = constraint.get("rhs", 0)
   def op = constraint.get("op", "<=")
   if op == "<=" { return total <= rhs }
   if op == "<" { return total < rhs }
   if op == ">=" { return total >= rhs }
   if op == ">" { return total > rhs }
   if op == "==" { return total == rhs }
   false
}

fn _finite_walk(list names, dict domains, int index, dict assignment,
   any predicate, dict state, int depth) any {
   if !_consume(state, depth) { return nil }
   if index >= names.len {
      if predicate(assignment) { return assignment }
      return nil
   }
   def name = names[index]
   def values = domains.get(name, [])
   mut i = 0
   while i < values.len {
      def found = _finite_walk(names, domains, index + 1,
         assignment.set(name, values[i]), predicate, state, depth + 1)
      if found != nil || !state.get("decided") { return found }
      i += 1
   }
   nil
}

;; Returns the result of the `finite` operation.
fn finite(dict domains, any predicate, int max_steps=100000,
   int max_nodes=100000, int max_depth=128, int max_variables=32,
   int max_memory=1000000) dict {
   def names = domains.keys()
   if max_steps <= 0 || max_nodes <= 0 || max_depth <= 0 ||
      max_variables <= 0 || max_memory <= 0 {
      return {"decided":false, "reason":"invalid budget", "found":false}
   }
   if names.len > max_variables {
      return {"decided":false, "reason":"variable limit", "found":false}
   }
   mut cells = names.len
   mut i = 0
   while i < names.len {
      def values = domains.get(names[i], [])
      if !is_list(values) {
         return {"decided":false, "reason":"invalid domain", "found":false}
      }
      cells += values.len
      i += 1
   }
   if cells > max_memory {
      return {"decided":false, "reason":"memory limit", "found":false}
   }
   mut state = {"decided":true, "reason":"exhausted", "steps":0,
      "nodes":0, "max_steps":max_steps, "max_nodes":max_nodes,
      "max_depth":max_depth, "memory":cells, "max_memory":max_memory}
   def assignment = _finite_walk(names, domains, 0, dict(16), predicate, state, 0)
   def found = assignment != nil
   if found { state["reason"] = "witness" }
   def evidence = {"domains":domains, "assignment":assignment,
      "exhausted":state.get("decided") && !found}
   return {"decided":state.get("decided"), "reason":state.get("reason"),
      "found":found, "assignment":assignment, "steps":state.get("steps"),
      "nodes":state.get("nodes"), "memory_cells":cells,
      "certificate":_certificate("finite", found, evidence)}
}

mut _linear_constraints = []
mut _linear_state = {}

fn _linear_predicate(dict assignment) bool {
   mut i = 0
   while i < _linear_constraints.len {
      if !_linear_holds(_linear_constraints[i], assignment, _linear_state) {
         return false
      }
      i += 1
   }
   true
}

;; Returns the result of the `linear` operation.
fn linear(list constraints, dict bounds, int max_steps=100000,
   int max_nodes=100000, int max_depth=128, int max_variables=32,
   int max_memory=1000000) dict {
   mut domains = dict(32)
   def names = bounds.keys()
   mut i = 0
   while i < names.len {
      def bound = bounds.get(names[i])
      if !is_list(bound) || bound.len != 2 || bound[1] < bound[0] {
         return {"decided":false, "reason":"invalid bound",
            "satisfiable":false}
      }
      mut values = list(bound[1] - bound[0] + 1)
      mut value = bound[0]
      while value <= bound[1] {
         values = values.append(value)
         value += 1
      }
      domains[names[i]] = values
      i += 1
   }
   _linear_constraints = constraints
   _linear_state = {"decided":true, "reason":"complete", "steps":0,
      "nodes":0, "max_steps":max_steps, "max_nodes":max_nodes,
      "max_depth":max_depth, "memory":0, "max_memory":max_memory}
   def result = finite(domains, _linear_predicate, max_steps, max_nodes,
      max_depth, max_variables, max_memory)
   if !_linear_state.get("decided") {
      result["decided"] = false
      result["reason"] = _linear_state.get("reason")
   }
   result["satisfiable"] = result.get("found")
   result["certificate"] = _certificate("linear",
      result.get("satisfiable"), {"constraints":constraints,
         "bounds":bounds, "assignment":result.get("assignment")})
   result
}

;; Returns the result of the `induction` operation.
fn induction(any predicate, int first, int last, int max_steps=100000,
   int max_depth=128, int max_nodes=100000, int max_memory=1000000,
   int max_variables=1) dict {
   if last < first || max_steps <= 0 || max_depth <= 0 || max_nodes <= 0 ||
      max_memory <= 0 || max_variables < 1 {
      return {"decided":false, "reason":"invalid budget", "valid":false}
   }
   def count = last - first + 1
   if count > max_memory {
      return {"decided":false, "reason":"memory limit", "valid":false}
   }
   mut checked = []
   mut n = first
   mut steps = 0
   while n <= last {
      if steps >= max_steps || checked.len >= max_nodes {
         return {"decided":false,
            "reason":steps >= max_steps ? "step limit" : "node limit",
            "valid":false, "checked":checked, "steps":steps}
      }
      if !predicate(n) {
         return {"decided":true, "reason":"counterexample", "valid":false,
            "counterexample":n, "checked":checked, "steps":steps + 1,
            "certificate":_certificate("induction", false,
               {"first":first, "last":last, "counterexample":n})}
      }
      checked = checked.append(n)
      steps += 1
      n += 1
   }
   def evidence = {"first":first, "last":last, "checked":checked}
   return {"decided":true, "reason":"bounded induction", "valid":true,
      "checked":checked, "steps":steps,
      "nodes":steps, "variables":1, "memory":checked.len,
      "certificate":_certificate("induction", true, evidence)}
}
