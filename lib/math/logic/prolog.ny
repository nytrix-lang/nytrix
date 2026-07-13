;; Keywords: logic prolog unification rules facts query backtracking
;; A bounded, self-hosted logic-programming engine.
module std.math.logic.prolog(
   variable, term, fact, rule, is_variable, is_term, is_clause,
   substitute, unify, bindings, query)

use std.core

;; Returns the result of the `variable` operation.
fn variable(str name) dict {
   assert(name.len > 0, "variable expects a non-empty name")
   return {"kind":"variable", "name":name}
}

;; Returns the result of the `term` operation.
fn term(str name, list args=[]) dict {
   assert(name.len > 0, "term expects a non-empty predicate name")
   return {"kind":"term", "name":name, "args":args}
}

;; Returns the result of the `fact` operation.
fn fact(dict head) dict {
   assert(is_term(head), "fact expects a term")
   return {"kind":"clause", "head":head, "body":[]}
}

;; Returns the result of the `rule` operation.
fn rule(dict head, list body) dict {
   assert(is_term(head), "rule head must be a term")
   mut i = 0
   while i < body.len {
      assert(is_term(body[i]), "rule body entries must be terms")
      i += 1
   }
   return {"kind":"clause", "head":head, "body":body}
}

;; Returns true when is variable.
fn is_variable(any value) bool {
   is_dict(value) && value.get("kind", "") == "variable" &&
      is_str(value.get("name", 0)) && value.get("name", "").len > 0
}

;; Returns true when is term.
fn is_term(any value) bool {
   if !is_dict(value) || value.get("kind", "") != "term" ||
      !is_str(value.get("name", 0)) || !is_list(value.get("args", 0)) {
      return false
   }
   true
}

;; Returns true when is clause.
fn is_clause(any value) bool {
   if !is_dict(value) || value.get("kind", "") != "clause" ||
      !is_term(value.get("head", 0)) || !is_list(value.get("body", 0)) {
      return false
   }
   mut i = 0
   def body = value.get("body")
   while i < body.len {
      if !is_term(body[i]) { return false }
      i += 1
   }
   true
}

fn _walk(any value, dict substitution) any {
   mut current = value
   mut depth = 0
   while is_variable(current) && substitution.contains(current.get("name")) {
      current = substitution.get(current.get("name"))
      depth += 1
      assert(depth <= 1024, "cyclic substitution")
   }
   current
}

;; Returns the result of the `substitute` operation.
fn substitute(any value, dict substitution) any {
   def walked = _walk(value, substitution)
   if is_variable(walked) { return walked }
   if !is_term(walked) { return walked }
   mut args = []
   mut i = 0
   def source = walked.get("args")
   while i < source.len {
      args = args.append(substitute(source[i], substitution))
      i += 1
   }
   term(walked.get("name"), args)
}

fn _occurs(str name, any value, dict substitution) bool {
   def walked = _walk(value, substitution)
   if is_variable(walked) { return walked.get("name") == name }
   if !is_term(walked) { return false }
   def args = walked.get("args")
   mut i = 0
   while i < args.len {
      if _occurs(name, args[i], substitution) { return true }
      i += 1
   }
   false
}

fn _unify(any left, any right, dict substitution) dict {
   def a = _walk(left, substitution)
   def b = _walk(right, substitution)
   if is_variable(a) {
      if is_variable(b) && a.get("name") == b.get("name") {
         return {"ok":true, "substitution":substitution}
      }
      if _occurs(a.get("name"), b, substitution) {
         return {"ok":false, "substitution":substitution, "reason":"occurs check"}
      }
      return {"ok":true,
         "substitution":substitution.set(a.get("name"), b)}
   }
   if is_variable(b) { return _unify(b, a, substitution) }
   if is_term(a) || is_term(b) {
      if !is_term(a) || !is_term(b) || a.get("name") != b.get("name") ||
         a.get("args").len != b.get("args").len {
         return {"ok":false, "substitution":substitution, "reason":"term mismatch"}
      }
      mut out = substitution
      mut i = 0
      while i < a.get("args").len {
         def step = _unify(a.get("args")[i], b.get("args")[i], out)
         if !step.get("ok") { return step }
         out = step.get("substitution")
         i += 1
      }
      return {"ok":true, "substitution":out}
   }
   if a == b { return {"ok":true, "substitution":substitution} }
   return {"ok":false, "substitution":substitution, "reason":"value mismatch"}
}

;; Returns the result of the `unify` operation.
fn unify(any left, any right, dict substitution={}) dict {
   _unify(left, right, substitution)
}

fn _fresh(any value, str suffix) any {
   if is_variable(value) { return variable(value.get("name") + suffix) }
   if !is_term(value) { return value }
   mut args = []
   mut i = 0
   def source = value.get("args")
   while i < source.len {
      args = args.append(_fresh(source[i], suffix))
      i += 1
   }
   term(value.get("name"), args)
}

fn _fresh_clause(dict clause, int stamp) dict {
   def suffix = "@" + to_str(stamp)
   mut body = []
   mut i = 0
   def source = clause.get("body")
   while i < source.len {
      body = body.append(_fresh(source[i], suffix))
      i += 1
   }
   rule(_fresh(clause.get("head"), suffix), body)
}

fn _solve(list knowledge, list goals, dict substitution, dict state,
   int depth) any {
   if depth > state.get("max_depth") {
      state["complete"] = false
      state["reason"] = "depth limit"
      return nil
   }
   if state.get("nodes") >= state.get("max_nodes") {
      state["complete"] = false
      state["reason"] = "node limit"
      return nil
   }
   state["nodes"] = state.get("nodes") + 1
   if depth > state.get("peak_depth") { state["peak_depth"] = depth }
   def cells = 1 + goals.len + substitution.len
   if state.get("memory_cells") + cells > state.get("max_memory_cells") {
      state["complete"] = false
      state["reason"] = "memory limit"
      return nil
   }
   state["memory_cells"] = state.get("memory_cells") + cells
   if state.get("memory_cells") > state.get("peak_memory_cells") {
      state["peak_memory_cells"] = state.get("memory_cells")
   }
   if state.get("steps") >= state.get("max_steps") {
      state["complete"] = false
      state["reason"] = "step limit"
      return nil
   }
   if state.get("solutions").len >= state.get("max_solutions") {
      state["complete"] = false
      state["reason"] = "solution limit"
      return nil
   }
   if goals.len == 0 {
      state["solutions"] = state.get("solutions").append(substitution)
      return nil
   }

   def goal = substitute(goals[0], substitution)
   def rest = goals.slice(1, goals.len, 1)
   mut i = 0
   while i < knowledge.len {
      if state.get("steps") >= state.get("max_steps") {
         state["complete"] = false
         state["reason"] = "step limit"
         return nil
      }
      state["steps"] = state.get("steps") + 1
      def clause = _fresh_clause(knowledge[i], state.get("steps"))
      def matched = _unify(goal, clause.get("head"), substitution)
      if matched.get("ok") {
         def next_goals = clause.get("body") + rest
         _solve(knowledge, next_goals, matched.get("substitution"), state,
            depth + 1)
      }
      if state.get("solutions").len >= state.get("max_solutions") {
         state["complete"] = false
         state["reason"] = "solution limit"
         return nil
      }
      i += 1
   }
   nil
}

;; Returns the result of the `bindings` operation.
fn bindings(list variables, dict substitution) dict {
   mut out = {}
   mut i = 0
   while i < variables.len {
      def v = variables[i]
      assert(is_variable(v), "bindings expects variables")
      out = out.set(v.get("name"), substitute(v, substitution))
      i += 1
   }
   out
}

fn _variables_add(list variables, dict value) list {
   mut i = 0
   while i < variables.len {
      if variables[i].get("name") == value.get("name") { return variables }
      i += 1
   }
   variables.append(value)
}

fn _variables_into(any value, list variables) list {
   if is_variable(value) { return _variables_add(variables, value) }
   if !is_term(value) { return variables }
   def args = value.get("args")
   mut i = 0
   while i < args.len {
      variables = _variables_into(args[i], variables)
      i += 1
   }
   variables
}

;; Returns the result of the `query` operation.
fn query(list knowledge, any goals, int max_steps=10000,
   int max_solutions=256, int max_depth=256, int max_variables=256,
   int max_memory_cells=1000000, int max_nodes=100000) dict {
   assert(max_steps > 0 && max_solutions > 0 && max_depth > 0 &&
      max_variables > 0 && max_memory_cells > 0 && max_nodes > 0,
      "query budgets must be positive")
   mut i = 0
   while i < knowledge.len {
      assert(is_clause(knowledge[i]), "query knowledge entries must be clauses")
      i += 1
   }
   def goal_list = is_list(goals) ? goals : [goals]
   i = 0
   while i < goal_list.len {
      assert(is_term(goal_list[i]), "query goals must be terms")
      i += 1
   }
   mut query_variables = []
   i = 0
   while i < goal_list.len {
      query_variables = _variables_into(goal_list[i], query_variables)
      i += 1
   }
   if query_variables.len > max_variables {
      return {"decided":false, "reason":"variable limit", "steps":0,
         "nodes":0, "peak_depth":0, "memory_cells":0, "peak_memory_cells":0,
         "answers":[], "solutions":[]}
   }
   mut state = {"steps":0, "max_steps":max_steps,
      "max_solutions":max_solutions, "solutions":[],
      "max_depth":max_depth, "peak_depth":0,
      "nodes":0, "max_nodes":max_nodes,
      "max_memory_cells":max_memory_cells, "memory_cells":0,
      "peak_memory_cells":0, "complete":true, "reason":"exhausted"}
   _solve(knowledge, goal_list, {}, state, 0)
   mut answers = []
   i = 0
   while i < state.get("solutions").len {
      answers = answers.append(bindings(query_variables, state.get("solutions")[i]))
      i += 1
   }
   return {"decided":state.get("complete"), "reason":state.get("reason"),
      "steps":state.get("steps"), "peak_depth":state.get("peak_depth"),
      "nodes":state.get("nodes"),
      "memory_cells":state.get("memory_cells"),
      "peak_memory_cells":state.get("peak_memory_cells"), "answers":answers,
      "solutions":state.get("solutions")}
}
