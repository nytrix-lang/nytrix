;; Keywords: logic rewrite normalize congruence bounded terms
;; Deterministic, bounded rewriting over std.math.logic.prolog terms.
module std.math.logic.rewrite(rule, is_rule, rewrite_once, normalize)

use std.core
use std.math.logic.prolog as prolog

fn rule(any pattern, any replacement) dict {
   return {"kind":"rewrite-rule", "pattern":pattern,
      "replacement":replacement}
}

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
   state["nodes"] = state.get("nodes") + 1
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

fn rewrite_once(any value, list rules, int max_steps=10000,
   int max_depth=128, int max_nodes=100000) dict {
   assert(max_steps > 0 && max_depth > 0 && max_nodes > 0,
      "rewrite budgets must be positive")
   _validate_rules(rules)
   mut state = {"decided":true, "reason":"complete", "steps":0,
      "nodes":0, "max_steps":max_steps, "max_depth":max_depth,
      "max_nodes":max_nodes}
   def result = _rewrite(value, rules, state, 0)
   return {"decided":state.get("decided"), "reason":state.get("reason"),
      "changed":result.get("changed"), "value":result.get("value"),
      "steps":state.get("steps"), "nodes":state.get("nodes")}
}

fn normalize(any value, list rules, int max_passes=256,
   int max_steps=10000, int max_depth=128, int max_nodes=100000) dict {
   assert(max_passes > 0 && max_steps > 0 && max_depth > 0 && max_nodes > 0,
      "normalize budgets must be positive")
   _validate_rules(rules)
   mut current = value
   mut passes = 0
   mut remaining_steps = max_steps
   mut remaining_nodes = max_nodes
   while passes < max_passes {
      def result = rewrite_once(current, rules, remaining_steps,
         max_depth, remaining_nodes)
      current = result.get("value")
      remaining_steps -= result.get("steps")
      remaining_nodes -= result.get("nodes")
      passes += 1
      if !result.get("decided") {
         return {"decided":false, "reason":result.get("reason"),
            "value":current, "passes":passes,
            "steps":max_steps - remaining_steps,
            "nodes":max_nodes - remaining_nodes}
      }
      if !result.get("changed") {
         return {"decided":true, "reason":"normal form", "value":current,
            "passes":passes, "steps":max_steps - remaining_steps,
            "nodes":max_nodes - remaining_nodes}
      }
      if remaining_steps <= 0 || remaining_nodes <= 0 {
         return {"decided":false,
            "reason":remaining_steps <= 0 ? "step limit" : "node limit",
            "value":current, "passes":passes,
            "steps":max_steps - remaining_steps,
            "nodes":max_nodes - remaining_nodes}
      }
   }
   return {"decided":false, "reason":"pass limit", "value":current,
      "passes":passes, "steps":max_steps - remaining_steps,
      "nodes":max_nodes - remaining_nodes}
}
