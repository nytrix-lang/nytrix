use std.math.logic as logic

def p = logic.prop_atom("p")
def q = logic.prop_atom("q")
def identity = logic.prop_implies(logic.prop_and(p, q), p)
def valid = logic.prop_tautology_report(identity)
assert(valid.get("decided") && valid.get("valid"), "and elimination tautology")
assert(valid.get("assignments_checked") == 4,
   "tautology report exposes bounded exhaustive work")

def invalid = logic.prop_tautology_report(logic.prop_implies(p, q))
assert(invalid.get("decided") && !invalid.get("valid"), "counterexample found")
assert(invalid.get("counterexample").get("p") &&
   !invalid.get("counterexample").get("q"), "counterexample assignment")
assert(invalid.get("assignments_checked") <= invalid.get("assignments_required"),
   "counterexample report exposes bounded work")

def simple = logic.prop_simplify(logic.prop_and(logic.prop_true(), p))
assert(logic.prop_eval(simple, {"p":true}), "simplification preserves meaning")
