use std.math.logic.prolog as prolog

def X = prolog.variable("X")
def Y = prolog.variable("Y")
def Z = prolog.variable("Z")
def Who = prolog.variable("Who")

def family = [
   prolog.fact(prolog.term("parent", ["alice", "bob"])),
   prolog.fact(prolog.term("parent", ["bob", "carol"])),
   prolog.rule(prolog.term("ancestor", [X, Y]),
      [prolog.term("parent", [X, Y])]),
   prolog.rule(prolog.term("ancestor", [X, Y]),
      [prolog.term("parent", [X, Z]), prolog.term("ancestor", [Z, Y])])
]

def result = prolog.query(family,
   prolog.term("ancestor", ["alice", Who]), 1000, 10)
assert(result.get("decided") && result.get("answers").len == 2,
   "recursive query completes with two answers")
assert(result.get("answers")[0].get("Who") == "bob" &&
   result.get("answers")[1].get("Who") == "carol",
   "query exposes user-variable answers")

def V = prolog.variable("V")
def cycle = prolog.unify(V, prolog.term("loop", [V]))
assert(!cycle.get("ok") && cycle.get("reason") == "occurs check",
   "unification rejects cyclic substitutions")

def bounded = prolog.query(family,
   prolog.term("ancestor", ["alice", Who]), 1, 10)
assert(!bounded.get("decided") && bounded.get("reason") == "step limit",
   "bounded query reports incomplete instead of guessing")

def depth_bounded = prolog.query(family,
   prolog.term("ancestor", ["alice", Who]), 1000, 10, 1)
assert(!depth_bounded.get("decided") &&
   depth_bounded.get("reason") == "depth limit",
   "recursive query reports depth exhaustion")

def memory_bounded = prolog.query(family,
   prolog.term("ancestor", ["alice", Who]), 1000, 10, 256, 256, 2)
assert(!memory_bounded.get("decided") &&
   memory_bounded.get("reason") == "memory limit",
   "query reports logical workspace exhaustion")

def Many = [prolog.variable("A"), prolog.variable("B")]
def variable_bounded = prolog.query([], prolog.term("pair", Many),
   1000, 10, 256, 1)
assert(!variable_bounded.get("decided") &&
   variable_bounded.get("reason") == "variable limit",
   "query reports variable-budget exhaustion")

def node_bounded = prolog.query(family,
   prolog.term("ancestor", ["alice", Who]), 1000, 10, 256, 256,
   1000000, 1)
assert(!node_bounded.get("decided") &&
   node_bounded.get("reason") == "node limit",
   "query reports node-budget exhaustion")
