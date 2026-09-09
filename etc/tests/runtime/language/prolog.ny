use std.core
use std.math.logic.prolog (variable, term, fact, rule, is_variable, is_term, is_clause, substitute, unify, bindings, query)

def X = variable("X")
def Y = variable("Y")
def Z = variable("Z")
def Who = variable("Who")
def test_rule = rule(term("ancestor", [X, Y]),
   [term("parent", [X, Y])])
assert(test_rule.get("kind", "") == "clause", "test_rule kind clause")
def family = [
   fact(term("parent", ["alice", "bob"])),
   fact(term("parent", ["bob", "carol"])),
   rule(term("ancestor", [X, Y]),
      [term("parent", [X, Y])]),
   rule(term("ancestor", [X, Y]),
      [term("parent", [X, Z]), term("ancestor", [Z, Y])])
]

def result = query(family,
   term("ancestor", ["alice", Who]), 1000, 10)
assert(result.get("decided") && result.get("answers").len == 2,
   "recursive query completes with two answers")
assert(result.get("answers")[0].get("Who") == "bob" &&
   result.get("answers")[1].get("Who") == "carol",
   "query exposes user-variable answers")
def V = variable("V")
def cycle = unify(V, term("loop", [V]))
assert(!cycle.get("ok") && cycle.get("reason") == "occurs check",
   "unification rejects cyclic substitutions")
def bounded = query(family,
   term("ancestor", ["alice", Who]), 1, 10)
assert(!bounded.get("decided") && bounded.get("reason") == "step limit",
   "bounded query reports incomplete instead of guessing")
def depth_bounded = query(family,
   term("ancestor", ["alice", Who]), 1000, 10, 1)
assert(!depth_bounded.get("decided") &&
   depth_bounded.get("reason") == "depth limit",
   "depth-bounded query reports depth limit")
def nodes_bounded = query(family,
   term("ancestor", ["alice", Who]), 1000, 10, 10, 10, 1000000, 1)
assert(!nodes_bounded.get("decided") &&
   nodes_bounded.get("reason") == "node limit",
   "node-bounded query reports node limit")
def cells_bounded = query(family,
   term("ancestor", ["alice", Who]), 1000, 10, 10, 10, 1, 1000)
assert(!cells_bounded.get("decided") &&
   cells_bounded.get("reason") == "memory limit",
   "memory-bounded query reports memory limit")
print("✓ logic-programming engine unit tests passed\n")
