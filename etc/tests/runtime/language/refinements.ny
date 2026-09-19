use std.core

;; ============================================================================
;; NEWT-01: Refinement types, composition, subtyping, ABI preservation
;; ============================================================================
refine Even = int where (v) { v % 2 == 0 }
refine Positive = int where (v) { v > 0 }
refine PositiveEven = Even where (v) { Positive(v) }

def Even x = 2
fn half(Even e) int { e / 2 }
assert(half(x) == 1, "half(Even)")

;; Subtyping: Even is accepted wherever int is expected (representation-preserving ABI)
fn add_one(int i) int { i + 1 }
assert(add_one(x) == 3, "Even <: int subtyping")

;; Composition of refinements
def PositiveEven pe = 4
assert(half(pe) == 2, "PositiveEven <: Even")

;; ============================================================================
;; NEWT-02: Existential refinements (comptime-only exists), membership folding
;; ============================================================================
refine EvenEx from int where (v) { exists(k) { v == 2 * k } }
refine Nat from int where (v) { v >= 0 }

def EvenEx six = 6
assert(EvenEx(6), "membership as a value: folds to true")
assert(!EvenEx(5), "negation folds to true")
static_assert(EvenEx(six), "folded existential admits witness")

;; ============================================================================
;; NEWT-03: Evidence-gated erasure (@pure @total)
;; ============================================================================
@pure @total
fn sha_block_len() int { 64 }

def erased_len = comptime{ sha_block_len() }
assert(erased_len == 64, "evidence-gated erasure of verified pure total function")

;; ============================================================================
;; NEWT-10: Dependent functions (requires preconditions & Fin<N>)
;; ============================================================================
fn lemma nonneg_fin(Fin<4> n) { n >= 0 }

fn natsum(int n) int requires n >= 0 {
  n * (n + 1) / 2
}

def Fin<4> fi = 2
def sum_res = natsum(fi)
assert(sum_res == 3, "natsum with Fin<4> argument")

;; ============================================================================
;; NEWT-11: Termination measures (decreases(n))
;; ============================================================================
fn countdown(int n) int decreases(n) {
  if n <= 0 {
    0
  } else {
    countdown(n - 1)
  }
}
assert(countdown(10) == 0, "recursive function with decreases measure")

;; ============================================================================
;; NEWT-13: Trait with comptime-checked law blocks
;; ============================================================================
trait Show {
  fn show(any v) str
}

struct Point { int x, int y }

impl Show for Point {
  fn show(p) str {
    "Point"
  }
  law { 1 == 1 }
}

def Point pt = Point(10, 20)
assert(show(pt) == "Point", "trait dispatch with verified law block")
