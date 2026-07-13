;; Keywords: logic rewrite normalize congruence bounded terms
;; Deterministic, bounded rewriting over std.math.logic.prolog terms.
module std.math.logic.rewrite(rule, is_rule, rewrite_once, normalize)

use std.core
use std.math.logic.prolog as prolog

;; Returns the result of the `rule` operation.
fn rule(any pattern, any replacement) dict {
   return {"kind":"rewrite-rule", "pattern":pattern,
      "replacement":replacement}
}

;; Returns true when is rule.
fn is_rule(any value) bool {
   is_dict(value) && value.get("kind", "") == "rewrite-rule" &&
      value.contains("pattern") && value.contains("replacement")
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
   if state.get("memory") >= state.get("max_memory") {
      state["decided"] = false
      state["reason"] = "memory limit"
      return false
   }
   state["nodes"] = state.get("nodes") + 1
   state["memory"] = state.get("memory") + 1
   true
}

fn _collect_variables(any value, dict seen, dict state, int depth) bool {
   if !_consume(state, depth) { return false }
   if prolog.is_variable(value) {
      seen[value.get("name")] = true
      if seen.len > state.get("max_variables") {
         state["decided"] = false
         state["reason"] = "variable limit"
         return false
      }
      return true
   }
   if !prolog.is_term(value) { return true }
   mut i = 0
   while i < value.get("args").len {
      if !_collect_variables(value.get("args")[i], seen, state, depth + 1) {
         return false
      }
      i += 1
   }
   true
}

fn _rewrite_children(any value, list rules, dict state, int depth) dict {
   if !_consume(state, depth) {
      return {"value":value, "changed":false}
   }
   if !prolog.is_term(value) {
      return {"value":value, "changed":false}
   }
   mut args = []
   mut changed = false
   mut i = 0
   def source = value.get("args")
   while i < source.len {
      def child = _rewrite(source[i], rules, state, depth + 1)
      args = args.append(child.get("value"))
      changed = changed || child.get("changed")
      if !state.get("decided") {
         return {"value":prolog.term(value.get("name"), args +
            source.slice(i + 1, source.len, 1)), "changed":changed}
      }
      i += 1
   }
   return {"value":prolog.term(value.get("name"), args), "changed":changed}
}

fn _rewrite(any value, list rules, dict state, int depth) dict {
   def children = _rewrite_children(value, rules, state, depth)
   mut current = children.get("value")
   if !state.get("decided") { return children }
   mut i = 0
   while i < rules.len {
      if state.get("steps") >= state.get("max_steps") {
         state["decided"] = false
         state["reason"] = "step limit"
         return {"value":current, "changed":children.get("changed")}
      }
      state["steps"] = state.get("steps") + 1
      def matched = prolog.unify(rules[i].get("pattern"), current)
      if matched.get("ok") {
         return {"value":prolog.substitute(rules[i].get("replacement"),
            matched.get("substitution")), "changed":true}
      }
      i += 1
   }
   return {"value":current, "changed":children.get("changed")}
}

fn _validate_rules(list rules) any {
   mut i = 0
   while i < rules.len {
      assert(is_rule(rules[i]), "rewrite rules must be created with rule")
      i += 1
   }
   nil
}

;; Returns the result of the `rewrite_once` operation.
fn rewrite_once(any value, list rules, int max_steps=10000,
   int max_depth=128, int max_nodes=100000, int max_variables=256,
   int max_memory=100000) dict {
   assert(max_steps > 0 && max_depth > 0 && max_nodes > 0 &&
      max_variables > 0 && max_memory > 0,
      "rewrite budgets must be positive")
   _validate_rules(rules)
   mut state = {"decided":true, "reason":"complete", "steps":0,
      "nodes":0, "max_steps":max_steps, "max_depth":max_depth,
      "max_nodes":max_nodes, "variables":0,
      "max_variables":max_variables, "memory":0,
      "max_memory":max_memory}
   mut seen = dict(16)
   _collect_variables(value, seen, state, 0)
   mut ri = 0
   while ri < rules.len && state.get("decided") {
      _collect_variables(rules[ri].get("pattern"), seen, state, 0)
      _collect_variables(rules[ri].get("replacement"), seen, state, 0)
      ri += 1
   }
   state["variables"] = seen.len
   def result = _rewrite(value, rules, state, 0)
   return {"decided":state.get("decided"), "reason":state.get("reason"),
      "changed":result.get("changed"), "value":result.get("value"),
      "steps":state.get("steps"), "nodes":state.get("nodes"),
      "variables":state.get("variables"), "memory":state.get("memory")}
}

;; Returns the result of the `normalize` operation.
fn normalize(any value, list rules, int max_passes=256,
   int max_steps=10000, int max_depth=128, int max_nodes=100000,
   int max_variables=256, int max_memory=100000) dict {
   assert(max_passes > 0 && max_steps > 0 && max_depth > 0 && max_nodes > 0 &&
      max_variables > 0 && max_memory > 0,
      "normalize budgets must be positive")
   _validate_rules(rules)
   mut current = value
   mut passes = 0
   mut remaining_steps = max_steps
   mut remaining_nodes = max_nodes
   mut remaining_memory = max_memory
   while passes < max_passes {
      def result = rewrite_once(current, rules, remaining_steps,
         max_depth, remaining_nodes, max_variables, remaining_memory)
      current = result.get("value")
      remaining_steps -= result.get("steps")
      remaining_nodes -= result.get("nodes")
      remaining_memory -= result.get("memory")
      passes += 1
      if !result.get("decided") {
         return {"decided":false, "reason":result.get("reason"),
            "value":current, "passes":passes,
            "steps":max_steps - remaining_steps,
            "nodes":max_nodes - remaining_nodes,
            "memory":max_memory - remaining_memory}
      }
      if !result.get("changed") {
         return {"decided":true, "reason":"normal form", "value":current,
            "passes":passes, "steps":max_steps - remaining_steps,
            "nodes":max_nodes - remaining_nodes,
            "memory":max_memory - remaining_memory}
      }
      if remaining_steps <= 0 || remaining_nodes <= 0 || remaining_memory <= 0 {
         return {"decided":false,
            "reason":remaining_steps <= 0 ? "step limit" :
               (remaining_nodes <= 0 ? "node limit" : "memory limit"),
            "value":current, "passes":passes,
            "steps":max_steps - remaining_steps,
            "nodes":max_nodes - remaining_nodes,
            "memory":max_memory - remaining_memory}
      }
   }
   return {"decided":false, "reason":"pass limit", "value":current,
      "passes":passes, "steps":max_steps - remaining_steps,
      "nodes":max_nodes - remaining_nodes,
      "memory":max_memory - remaining_memory}
}
