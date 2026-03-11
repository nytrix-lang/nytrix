;; Keywords: hnp hidden-number-problem lattice leakage lsb msb nonce recovery
;; Reference:
;; - https://crypto.stanford.edu/~dabo/pubs/papers/dhmsb.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
module std.math.crypto.hnp(hnp, hnp_centered_mod, hnp_leak_bound, hnp_embedding, hnp_default_sample_counts, hnp_partition_samples, hnp_bound_constraint, hnp_replace_samples, hnp_prescreen_target, hnp_lsb_lattice_eliminate_alpha, hnp_lsb_lattice_increase_volume, hnp_alpha_from_target, hnp_check_alpha, hnp_linear_predicate_eliminate_alpha, hnp_recover, hnp_lattice, hnp_recover_x, hnp_extract_x)
use std.math.crypto.hnp.hnp
