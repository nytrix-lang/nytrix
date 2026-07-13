;; Keywords: law certificate algebra property bounded counterexample
;; Certify laws over the values and operations of existing modules.
module std.math.logic.laws(check, unary, binary, ternary)

use std.core
use std.core.reflect as reflect
use std.math.logic.certificate as cert

def LAW_VERSION = "std.math.logic.laws@1"

fn _invalid(str reason) dict {
   {"decided":false, "valid":false, "reason":reason,
      "cases_checked":0}
}

;; Returns the result of the `check` operation.
fn check(str domain, str law, list cases, any left, any right, any equal,
   int max_steps=100000, int max_nodes=100000, int max_depth=128,
   int max_variables=256, int max_memory=100000) dict {
   if domain == "" || law == "" { return _invalid("invalid identity") }
   if max_steps <= 0 || max_nodes <= 0 || max_depth <= 0 ||
      max_variables <= 0 || max_memory <= 0 {
      return _invalid("invalid budget")
   }
   if cases.len > max_variables { return _invalid("variable limit") }

   mut evidence = []
   mut i = 0
   mut steps = 0
   mut memory = 0
   while i < cases.len {
      if steps + 3 > max_steps { return _invalid("step limit") }
      if i + 1 > max_nodes { return _invalid("node limit") }
      if 1 > max_depth { return _invalid("depth limit") }
      def lhs = left(cases[i])
      def rhs = right(cases[i])
      def same = equal(lhs, rhs)
      def row = [reflect.repr(cases[i]), reflect.repr(lhs),
         reflect.repr(rhs), same]
      memory += reflect.repr(row).len
      if memory > max_memory { return _invalid("memory limit") }
      evidence = evidence.append(row)
      steps += 3
      if !same {
         return {"decided":true, "valid":false,
            "reason":"counterexample", "domain":domain, "law":law,
            "counterexample":cases[i], "left":lhs, "right":rhs,
            "cases_checked":i + 1, "steps":steps, "memory":memory}
      }
      i += 1
   }

   def dependency = to_str(hash(reflect.repr(
      [domain, law, evidence, max_steps, max_nodes, max_depth,
         max_variables, max_memory])))
   def certificate = cert.envelope("T", LAW_VERSION, dependency).merge({
      "domain":domain, "law":law, "evidence":evidence,
      "cases_checked":i})
   return {"decided":true, "valid":true, "reason":"complete",
      "domain":domain, "law":law, "cases_checked":i, "steps":steps,
      "memory":memory, "certificate":certificate}
}

;; Returns the result of the `unary` operation.
fn unary(str domain, str law, list values, any left, any right, any equal,
   int max_steps=100000, int max_nodes=100000, int max_depth=128,
   int max_variables=256, int max_memory=100000) dict {
   check(domain, law, values, left, right, equal, max_steps, max_nodes,
      max_depth, max_variables, max_memory)
}

;; Returns the result of the `binary` operation.
fn binary(str domain, str law, list pairs, any left, any right, any equal,
   int max_steps=100000, int max_nodes=100000, int max_depth=128,
   int max_variables=256, int max_memory=100000) dict {
   check(domain, law, pairs, left, right, equal, max_steps, max_nodes,
      max_depth, max_variables, max_memory)
}

;; Returns the result of the `ternary` operation.
fn ternary(str domain, str law, list triples, any left, any right, any equal,
   int max_steps=100000, int max_nodes=100000, int max_depth=128,
   int max_variables=256, int max_memory=100000) dict {
   check(domain, law, triples, left, right, equal, max_steps, max_nodes,
      max_depth, max_variables, max_memory)
}
