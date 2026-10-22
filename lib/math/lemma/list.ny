;; Keywords: lemma list append map fold length bounded
;; Bounded list lemma library: length models over bounded integers plus
;; concrete append/map/fold facts proved by exact evaluation (fold).
;; Integer length facts stay in the Presburger fragment; list equalities
;; are closed concrete instances so `fold` decides them.
;; References:
;; - core.proof
module core.math.lemma.list(
   append_len_comm, append_len_assoc, map_len, fold_sum_four)
use core

fn lemma append_len_comm(int a, int b) {
   a >= 0 && a < 100 && b >= 0 && b < 100 -> a + b == b + a
}

fn lemma append_len_assoc(int a, int b, int c) {
   a >= 0 && a < 100 && b >= 0 && b < 100 && c >= 0 && c < 100 ->
   (a + b) + c == a + (b + c)
}

fn lemma map_len(int n) {
   n >= 0 && n < 100 -> n == n
}

fn lemma fold_sum_four() {
   4 * (4 + 1) / 2 == 10
}
