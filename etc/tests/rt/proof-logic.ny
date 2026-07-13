use std.math.logic as logic
use std.math.logic.prolog as prolog
use std.math.logic.solve as solve
use std.math.logic.certificate as certificate
use std.math.logic.index as proof_index
use std.math.logic.laws as laws
use std.math.logic.commands as commands
use std.os
use std.os.path as ospath

def p = logic.prop_atom("p")
def q = logic.prop_atom("q")
def identity = logic.prop_implies(logic.prop_and(p, q), p)
def valid = logic.prop_tautology_report(identity)
assert(valid.get("decided") && valid.get("valid"), "and elimination tautology")
assert(valid.get("assignments_checked") == 4,
   "tautology report exposes bounded exhaustive work")
assert(logic.prop_tautology_report(identity, 0).get("reason") == "variable limit",
   "proposition variable budget is explicit")
assert(logic.prop_tautology_report(identity, 16, 1).get("reason") == "step limit",
   "proposition step budget is explicit")
assert(logic.prop_tautology_report(identity, 16, 1000, 1).get("reason") == "depth limit",
   "proposition depth budget is explicit")
assert(logic.prop_tautology_report(identity, 16, 1000, 128, 1).get("reason") == "node limit",
   "proposition node budget is explicit")
assert(logic.prop_tautology_report(identity, 16, 1000, 128, 100, 1).get("reason") == "memory limit",
   "proposition memory budget is explicit")

def identity_certificate = logic.certificate(identity)
assert(logic.check_certificate(identity_certificate),
   "compact checker accepts module-produced certificate")
assert(!logic.check_certificate(identity_certificate.set("envelope_digest", 0)),
   "compact checker rejects tampered envelope digest")

def a = prolog.term("a")
def b = prolog.term("b")
def fa = prolog.term("f", [a])
def fb = prolog.term("f", [b])
def cc = solve.congruence([[a, b]], fa, fb)
assert(cc.get("decided") && cc.get("equal") &&
   is_dict(cc.get("certificate")), "bounded congruence closure certificate")

def arithmetic = solve.arithmetic(prolog.term("mul", [
   prolog.term("add", [2, 3]), 4]))
assert(arithmetic.get("decided") && arithmetic.get("value") == 20,
   "bounded arithmetic normalization certificate")

fn finite_predicate(dict values) bool {
   values.get("x") + values.get("y") == 5
}
def finite = solve.finite({"x":[1, 2, 3], "y":[1, 2, 3]}, finite_predicate)
assert(finite.get("decided") && finite.get("found"),
   "bounded finite search certificate")

def linear = solve.linear([
   {"coefficients":{"x":1, "y":1}, "op":"==", "rhs":5},
   {"coefficients":{"x":1}, "op":">=", "rhs":2}],
   {"x":[0, 5], "y":[0, 5]})
assert(linear.get("decided") && linear.get("satisfiable"),
   "bounded linear arithmetic certificate")

fn induction_predicate(int n) bool { n * (n + 1) % 2 == 0 }
def induction = solve.induction(induction_predicate, 0, 32)
assert(induction.get("decided") && induction.get("valid"),
   "bounded induction certificate")

def exhausted = solve.finite({"x":[1, 2, 3]}, finite_predicate,
   1, 100, 10, 4, 100)
assert(!exhausted.get("decided") && exhausted.get("reason") == "step limit",
   "solver exhaustion is explicit")

def index_path = ospath.join(os.temp_dir(), "ny-proof-index-" +
   to_str(os.pid()) + ".json")
def indexed_certificate = certificate.envelope("T", "proof-test@1",
   "proof-dependencies@1")
mut index = proof_index.open(index_path, "proof-test@1",
   "proof-dependencies@1")
assert(proof_index.put(index, indexed_certificate) && proof_index.save(index),
   "certificate index persists atomically")
index = proof_index.open(index_path, "proof-test@1", "proof-dependencies@1")
assert(proof_index.lookup(index, "T") != nil && proof_index.size(index) == 1,
   "certificate index reloads matching entry")
def stale = proof_index.open(index_path, "proof-test@2", "proof-dependencies@1")
assert(proof_index.lookup(stale, "T") == nil && proof_index.size(stale) == 0,
   "certificate index rejects stale module version")
unwrap(os.file_remove(index_path))

fn law_add_left(list values) any { (values[0] + values[1]) + values[2] }
fn law_add_right(list values) any { values[0] + (values[1] + values[2]) }
fn law_equal(any left, any right) bool { left == right }
def integer_law = laws.ternary("integer", "addition-associative",
   [[-4, 2, 9], [0, 0, 0], [7, 11, -3]], law_add_left,
   law_add_right, law_equal)
assert(integer_law.get("decided") && integer_law.get("valid") &&
   certificate.check(integer_law.get("certificate")),
   "existing-domain laws emit compact certificates")

fn law_pair_left(list values) any { values[0] + values[1] }
fn law_bad_right(list values) any { values[0] + values[1] + 1 }
def rejected_law = laws.binary("integer", "deliberately-false",
   [[1, 2]], law_pair_left, law_bad_right, law_equal)
assert(rejected_law.get("decided") && !rejected_law.get("valid") &&
   rejected_law.get("reason") == "counterexample",
   "law certificates retain concrete counterexamples")
assert(laws.binary("integer", "budget", [[1, 2]], law_pair_left,
   law_bad_right, law_equal, 1).get("reason") == "step limit",
   "law checking is explicitly bounded")

def reasoning_commands = commands.standard_registry()
def command_result = commands.run(reasoning_commands, "arithmetic", [
   prolog.term("add", [20, 22])])
assert(command_result.get("decided") && command_result.get("value") == 42,
   "module reasoning commands expand through the syntax registry")
assert(commands.metadata(reasoning_commands, "arithmetic").get("kind") ==
   "reasoning-command" && commands.names(reasoning_commands)[0] == "arithmetic",
   "reasoning command metadata is deterministic and discoverable")

def comptime_command = comptime {
   def registry = commands.standard_registry()
   return commands.run(registry, "arithmetic", [
      prolog.term("mul", [6, 7])])
}
assert(comptime_command.get("value") == 42,
   "module reasoning commands cross the comptime boundary by value")

comptime template reasoning_export(name) {
   fn ${name}(any expression) any {
      commands.run(commands.standard_registry(), "arithmetic", [expression])
   }
}

module GeneratedReasoning generated from ReasoningCommands {
   export core(generated_arithmetic)
   emit reasoning_export(generated_arithmetic)
}

assert(GeneratedReasoning.generated_arithmetic(
   prolog.term("sub", [44, 2])).get("value") == 42,
   "generated modules expose ordinary reasoning APIs")

def invalid = logic.prop_tautology_report(logic.prop_implies(p, q))
assert(invalid.get("decided") && !invalid.get("valid"), "counterexample found")
assert(invalid.get("counterexample").get("p") &&
   !invalid.get("counterexample").get("q"), "counterexample assignment")
assert(invalid.get("assignments_checked") <= invalid.get("assignments_required"),
   "counterexample report exposes bounded work")

def simple = logic.prop_simplify(logic.prop_and(logic.prop_true(), p))
assert(logic.prop_eval(simple, {"p":true}), "simplification preserves meaning")
