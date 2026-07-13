use std.math.logic.prolog as prolog
use std.math.logic.rewrite as rewrite

def X = prolog.variable("X")
def zero = prolog.term("zero")
def rules = [
   rewrite.rule(prolog.term("add", [X, zero]), X),
   rewrite.rule(prolog.term("add", [zero, X]), X)
]

def expression = prolog.term("add", [zero,
   prolog.term("add", ["value", zero])])
def result = rewrite.normalize(expression, rules)
assert(result.get("decided") && result.get("reason") == "normal form",
   "bounded rewrite reaches a normal form")
assert(result.get("value") == "value",
   "rewriting descends through congruent term children")

def bounded = rewrite.normalize(expression, rules, 10, 1, 128, 1000)
assert(!bounded.get("decided") && bounded.get("reason") == "step limit",
   "rewrite exhaustion is explicit")

def depth_bounded = rewrite.normalize(expression, rules, 10, 1000, 1)
assert(!depth_bounded.get("decided") &&
   depth_bounded.get("reason") == "depth limit",
   "rewrite depth exhaustion is explicit")

def node_bounded = rewrite.normalize(expression, rules, 10, 1000, 128, 1)
assert(!node_bounded.get("decided") &&
   node_bounded.get("reason") == "node limit",
   "rewrite node exhaustion is explicit")

def memory_bounded = rewrite.normalize(expression, rules, 10, 1000, 128,
   1000, 256, 1)
assert(!memory_bounded.get("decided") &&
   memory_bounded.get("reason") == "memory limit",
   "rewrite memory exhaustion is explicit")

def Y = prolog.variable("Y")
def variable_rules = rules + [rewrite.rule(prolog.term("pair", [X, Y]), X)]
def variable_bounded = rewrite.normalize(expression, variable_rules, 10,
   1000, 128, 1000, 1, 1000)
assert(!variable_bounded.get("decided") &&
   variable_bounded.get("reason") == "variable limit",
   "rewrite variable exhaustion is explicit")
