<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"learn","order":190,"summary":"Executable C validation for NYIR rewrites: invariant fault injection, exhaustive rule checks, and seeded differential testing."} -->
# Compiler verification in C

Verification work in this repository is implemented in C and lives beside the
regression tests, not in a separate proof assistant. There is no Lean package
and no proof obligation that requires one: every claim below is produced by
running code that is part of the normal `ny-full` binary.

The reason is architectural. Production compilation must not depend on a second
toolchain, and a theorem about a separate model says nothing about the C code
that actually runs unless the two are connected. Instead, each mechanism
compares the real implementation against the real reference interpreter:

| Mechanism | Establishes | Where |
| --- | --- | --- |
| Fault injection | The structural validator rejects every injected IR defect | `src/code/test/backend/core/invariants.c` |
| Bounded exhaustive validation | Two IR forms agree on every value in a fixed domain | `src/code/backend/passes/verify/rules.c` |
| Seeded differential testing | Unoptimized and optimized IR agree on generated programs | `src/code/backend/passes/verify/differential.c` |
| Pass translation validation | A real pass fires, stays valid, kills its target operation and preserves results | `src/code/backend/passes/verify/passes.c` |
| Machine-IR fault injection | The machine verifier rejects undefined vregs, wrong classes, missing terminators and dangling targets | `src/code/test/backend/lower/machine/mach_invariants.c` |

Run each suite through the backend test runner.

```
build/release/ny-full --backend-test=core/invariants
build/release/ny-full --backend-test=passes/verify/rules
build/release/ny-full --backend-test=passes/verify/differential
build/release/ny-full --backend-test=passes/verify/passes
build/release/ny-full --backend-test=lower/machine/mach_invariants
```

## What each suite establishes

### Structural invariants (`invariants.c`)

Ten cases: one control that must verify and evaluate, and nine functions with
exactly one injected defect each. The defects are an undefined operand, a
use before its definition, a duplicate SSA definition, a branch to a
non-existent label, a missing terminator, an `NYIR_ADD_F64` over integer
operands, a use not dominated by its definition, a PHI edge naming a
non-predecessor, and a PHI with a duplicate predecessor. The control also
evaluates to its expected value, so "the validator accepts valid IR" is checked
rather than assumed.

A verifier that accepts an invalid function is worse than no verifier, because
the defect reaches machine code; this suite is the executable statement of that
rule.

### Rule validation (`rules.c`)

Twelve scalar rewrites are each built twice, once in the pre-rewrite form and
once in the form the optimizer claims is equivalent. Both are evaluated over a
21-value domain that includes `0`, `±1`, powers of two and their neighbours,
`2^32`, `INT64_MAX` and `INT64_MIN`. Every result must match: 252 comparisons.
The rules cover additive and multiplicative identities, `mul` → `shl`, `x - x`,
`x & x`, `x | x`, `x ^ x`, a zero shift, `x / 1`, mask combination, constant
reassociation, and a constant `select`.

The last case is deliberately negative: `x / 4` and `x >> 2` must disagree on
the domain, and do, on three values. The optimizer only performs that rewrite
under a proven non-negative dividend, so the disagreement is the counterexample
that keeps the guard load-bearing. If the two ever agree everywhere, the test
fails because the guard has stopped meaning anything.

### Differential testing (`differential.c`)

300 seeds, each a random expression of depth five over `local0` and small
constants, using add, sub, mul, and, or, xor, `shl` and `sar` with masked shift
counts and no division. Each program is cloned and the clone runs the real
scalar pipeline (e-graph, constant folding, peephole, CSE) twice, verifying after
every stage. Both forms must agree on all 19 domain inputs, 5,700 comparisons.

Three outcomes are distinguished, because collapsing them hides defects:

- `CASE_MISMATCH` — the optimizer changed an answer. This is a semantic defect.
- `CASE_INVALID_IR` — a pass produced IR the verifier rejects. This is a
  structural defect.
- `CASE_HARNESS_ERROR` — build, clone or evaluation failed. This is neither and
  is never reported as a miscompile.

On a mismatch the same seed is retried at depths one through four and the first
failing depth is reported, so the reproducer is the smallest tree the seed
admits rather than the full-depth random one. Operations that can trap are
excluded so a disagreement is a compiler defect and not an undefined-behavior
argument.

### Pass translation validation (`passes.c`)

Validating rule pairs is not the same as validating a pass: a pass that does
nothing agrees with its own input, so an equality-only test passes vacuously.
Each of five real passes therefore has to satisfy four conditions at once:

1. the input verifies and evaluates;
2. the pass reports a change;
3. the output verifies and the targeted operation is gone or dead;
4. the output evaluates identically on all 19 domain inputs.

Condition 3 uses two metrics because passes differ in how they finish. Constant
folding, peephole and CSE delete the operation, so its occurrence count must
drop. Copy propagation and the e-graph rewire every use and leave the old node
for later DCE, so the metric is the number of instances whose value still has a
user; that must drop. A pass that stops firing fails the case instead of
passing because nothing broke.

### Machine-IR invariants (`mach_invariants.c`)

The same contract one layer down, for functions handed to register allocation
and the object encoder. The control is a mixed-class function that must verify,
then five single-defect corruptions that must be rejected: an operand naming an
unallocated vreg, an operand class that disagrees with the vreg's declared type,
a block that falls off its end without a terminator, a branch to a block that
does not exist, and two blocks claiming the same label (rejected at
construction, before the verifier runs). The observed diagnostics name the
instruction index, so a failure points at the malformed instruction rather than
at the function.

## Coverage matrix

| Transformation | Evidence | Level |
| --- | --- | --- |
| `nyr_const_fold` | `passes.c` const-fold | Validated |
| `nyr_peephole` | `passes.c` peephole-add-zero | Validated |
| `nyr_egraph_rewrite` | `passes.c` egraph-mul-pow2, `rules.c` | Validated |
| `nyr_cse` | `passes.c` cse-redundant-add | Validated |
| `nyr_copy_prop` | `passes.c` copy-prop | Validated |
| `narrow`, `sccp`, `gvn_pre`, `strength_reduce`, `select_chain_fold`, loop/memory/vector passes | Existing regression tests only | Tested |
| Instruction selection and encoding | None | Untested here |

"Validated" means the pass fired and the checked cases preserved behavior over
the stated domain. It does not mean every rule the pass contains is covered:
the e-graph, for instance, reaches `Validated` only for the multiply-to-shift
case in `passes.c` plus the twelve pairs in `rules.c`.

## Claim ladder

Reports about a transformation should use these levels and no stronger word:

- **Checked** — the structural validator accepts the pass output.
- **Validated** — pre- and post-rewrite forms agree on an enumerated domain.
- **Differentially tested** — generated programs agree over a seeded corpus.
- **Proved** — not claimed anywhere in this repository today.

None of these is a proof of the C implementation, and none covers inputs outside
the stated domain. The repository does not claim machine-code-level refinement.

## Trusted base

- `nyr_ir_verify` — parses and checks the IR, including dominance and PHI edges.
- `nyr_ir_eval` — the reference interpreter both forms are compared through.
- The fixed domains and the generator's masking rules.
- The pass entry points (`nyr_egraph_rewrite`, `nyr_const_fold`,
  `nyr_peephole`, `nyr_cse`), which are the production implementations.

A defect in the interpreter or the verifier would weaken every claim above; that
is why `invariants.c` tests the verifier itself, and why the differential domain
includes the integer boundary values where an interpreter is most likely to be
wrong.

## Not covered

- Memory, aliasing, calls, traps and floating point. The suites are scalar
  integer only, and the rules under test are the ones with no side conditions
  beyond arithmetic.
- Instruction selection and machine-code emission. Nothing here checks that the
  emitted bytes refine the IR operation.
- The parser and source text; see the conformance work in
  [executed C verification](../spec/nyc.md#verification-evidence).
- Any transformation not listed in `rules.c`. A pass that is not in the table
  above has no validation evidence, whatever its regression tests show.

The next mechanisms worth adding, in order of value:

- A per-pass certificate or independent re-check of pass output against the
  rule set, so passes outside the five validated above get structural evidence
  instead of only regression tests.
- Memory and alias rules, which need a memory model in the reference
  interpreter before load/store rewrites can be validated at all.
- Machine-code translation validation: decode emitted bytes and check them
  against the machine operation model. Nothing in this directory covers
  encoding today, and no claim is made about it.
