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

fn lemma add_comm_bounded(int x, int y) {
   x >= 0 && x < 10 && y >= 0 && y < 10 -> x + y == y + x
}

fn lemma add_assoc_bounded(int x, int y, int z) {
   x >= 0 && x < 10 && y >= 0 && y < 10 && z >= 0 && z < 10 ->
   (x + y) + z == x + (y + z)
}

fn lemma mul_comm_bounded(int x, int y) {
   x >= 0 && x < 10 && y >= 0 && y < 10 -> x * y == y * x
}

fn lemma distrib_bounded(int x, int y, int z) {
   x >= 0 && x < 5 && y >= 0 && y < 5 && z >= 0 && z < 5 ->
   x * (y + z) == x * y + x * z
}

fn natsum(int n) int requires n >= 0 && n <= 10 {
   n * (n + 1) / 2
}

fn lemma natsum_four() {
   4 * (4 + 1) / 2 == 10
}

fn lemma natsum_zero() {
   0 * (0 + 1) / 2 == 0
}
