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
