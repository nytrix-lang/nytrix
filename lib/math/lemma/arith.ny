;; Keywords: lemma arithmetic assoc comm distrib natsum bounded
;; Bounded arithmetic lemma library: assoc/comm/distrib + natsum closed form.
;; Every lemma is bounded (0..N) so the independent C checker decides it;
;; unbounded forms stay unknown by design. Each lemma is `fn lemma` + `prove`.
;; References:
;; - core.proof (budgets 8 vars, 32 cons, depth 64)
module core.math.lemma.arith(
   add_comm_bounded, add_assoc_bounded, mul_comm_bounded,
   distrib_bounded, natsum_four, natsum_zero)
use core

;; Addition commutes for operands below 10.
fn lemma add_comm_bounded(int x, int y) {
   x >= 0 && x < 10 && y >= 0 && y < 10 -> x + y == y + x
}

;; Addition associates for operands below 10.
fn lemma add_assoc_bounded(int x, int y, int z) {
   x >= 0 && x < 10 && y >= 0 && y < 10 && z >= 0 && z < 10 ->
   (x + y) + z == x + (y + z)
}

;; Multiplication commutes for operands below 10.
fn lemma mul_comm_bounded(int x, int y) {
   x >= 0 && x < 10 && y >= 0 && y < 10 -> x * y == y * x
}

;; Multiplication distributes over addition for operands below 5.
fn lemma distrib_bounded(int x, int y, int z) {
   x >= 0 && x < 5 && y >= 0 && y < 5 && z >= 0 && z < 5 ->
   x * (y + z) == x * y + x * z
}

fn natsum(int n) int requires n >= 0 && n <= 10 {
   "Returns the sum of the integers from 0 through `n`."
   n * (n + 1) / 2
}

;; The closed form gives 10 for n = 4.
fn lemma natsum_four() {
   4 * (4 + 1) / 2 == 10
}

;; The closed form gives 0 for n = 0.
fn lemma natsum_zero() {
   0 * (0 + 1) / 2 == 0
}
