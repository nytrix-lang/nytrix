;; Keywords: hnp hidden-number-problem lattice leakage lsb msb nonce recovery math crypto
;; Reference:
;; - https://crypto.stanford.edu/~dabo/pubs/papers/dhmsb.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
;; References:
;; - std.math.crypto
module std.math.crypto.hnp(hnp, hnp_centered_mod, hnp_leak_bound, hnp_embedding, hnp_default_sample_counts, hnp_partition_samples, hnp_bound_constraint, hnp_replace_samples, hnp_prescreen_target, hnp_lsb_lattice_eliminate_alpha, hnp_lsb_lattice_increase_volume, hnp_alpha_from_target, hnp_check_alpha, hnp_linear_predicate_eliminate_alpha, hnp_recover, hnp_lattice, hnp_recover_x, hnp_extract_x)
use std.core
use std.math.nt
use std.math.crypto.hnp.hnp

#main {
   def q = Z(101)
   assert(hnp_centered_mod(105, q) == 4, "hnp_centered_mod positive")
   assert(hnp_centered_mod(-5, q) == -5, "hnp_centered_mod negative")
   assert(hnp_centered_mod(152, q) == -50, "hnp_centered_mod wraps to negative")
   def bound = hnp_leak_bound(q, 3)
   assert(bound == q / (Z(1) << 4), "hnp_leak_bound")
   def scale = hnp_embedding(q, 3)
   assert(scale > 0, "hnp_embedding positive")
   def counts = hnp_default_sample_counts(8, 16, 0)
   assert(counts.len == 4, "hnp_default_sample_counts returns 4 counts")
   assert(counts[0] == 16, "hnp_default_sample_counts lattice count")
   def groups = hnp_partition_samples([[1, 2], [3, 4], [5, 6], [7, 8]], counts)
   assert(groups.len == 4, "hnp_partition_samples 4 groups")
   print("✓ std.math.crypto.hnp self-test passed")
}
