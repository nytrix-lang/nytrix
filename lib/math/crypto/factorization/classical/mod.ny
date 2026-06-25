;; Keywords: factorization classical qs mpqs siqs gf2 dixon
;; References: std.math.crypto.factorization.classical.dixon std.math.crypto.factorization.classical.qs std.math.crypto.factorization.classical.gf2
module std.math.crypto.factorization.classical(dixon_factor, euler_factor, gf2_nullspace, gf2_nullspace_report, sparse_gf2_nullspace, sparse_gf2_nullspace_report, packed_gf2_nullspace, packed_gf2_nullspace_report, sparse_gf2_matvec_report, sparse_gf2_normal_matvec_report, packed_gf2_matvec_report, packed_gf2_normal_matvec_report, gf2_matrix_precondition_report, gf2_dependency_candidates, gf2_dependency_candidates_report, gf2_dependency_pipeline_report, block_lanczos_gf2_nullspace, block_lanczos_gf2_report, block_wiedemann_gf2_nullspace, block_wiedemann_gf2_report, qs_relation_filter_report, qs_multiplier_report, mpqs_multiplier_report, siqs_polynomial_report, siqs_cutoff_tune_report, siqs_relation_report, quadratic_sieve_factor, quadratic_sieve_factor_report, siqs_factor, siqs_factor_report, qs_large_prime_filter_report, qs_batch_cofactor_report, mpqs_sieve_parameter_report, mpqs_source_work_plan_report, mpqs_work_plan_report, mpqs_byte_sieve_report, mpqs_a_divisor_cycle_report, mpqs_source_factor, mpqs_source_factor_report, mpqs_factor, mpqs_factor_report)
use std.math.crypto.factorization.classical.dixon as dixon

def dixon_factor = dixon.dixon_factor
def euler_factor = dixon.euler_factor
use std.math.crypto.factorization.classical.gf2 as gf2

def block_lanczos_gf2_nullspace = gf2.block_lanczos_gf2_nullspace
def block_lanczos_gf2_report = gf2.block_lanczos_gf2_report
def block_wiedemann_gf2_nullspace = gf2.block_wiedemann_gf2_nullspace
def block_wiedemann_gf2_report = gf2.block_wiedemann_gf2_report
def gf2_dependency_candidates = gf2.gf2_dependency_candidates
def gf2_dependency_candidates_report = gf2.gf2_dependency_candidates_report
def gf2_dependency_pipeline_report = gf2.gf2_dependency_pipeline_report
def gf2_matrix_precondition_report = gf2.gf2_matrix_precondition_report
def gf2_nullspace = gf2.gf2_nullspace
def gf2_nullspace_report = gf2.gf2_nullspace_report
def packed_gf2_matvec_report = gf2.packed_gf2_matvec_report

fn packed_gf2_normal_matvec_report(list rows, list vector, int width=0, bool sparse=false) dict {
   gf2.packed_gf2_normal_matvec_report(rows, vector, width, sparse)
}

def packed_gf2_nullspace = gf2.packed_gf2_nullspace
def packed_gf2_nullspace_report = gf2.packed_gf2_nullspace_report
def sparse_gf2_matvec_report = gf2.sparse_gf2_matvec_report

fn sparse_gf2_normal_matvec_report(list sparse_rows, list vector, int width=0) dict {
   gf2.sparse_gf2_normal_matvec_report(sparse_rows, vector, width)
}

def sparse_gf2_nullspace = gf2.sparse_gf2_nullspace
def sparse_gf2_nullspace_report = gf2.sparse_gf2_nullspace_report
use std.math.crypto.factorization.classical.qs as qs

def mpqs_a_divisor_cycle_report = qs.mpqs_a_divisor_cycle_report
def mpqs_byte_sieve_report = qs.mpqs_byte_sieve_report
def mpqs_factor = qs.mpqs_factor
def mpqs_factor_report = qs.mpqs_factor_report
def mpqs_multiplier_report = qs.mpqs_multiplier_report
def mpqs_sieve_parameter_report = qs.mpqs_sieve_parameter_report
def mpqs_source_factor = qs.mpqs_source_factor
def mpqs_source_factor_report = qs.mpqs_source_factor_report
def mpqs_source_work_plan_report = qs.mpqs_source_work_plan_report
def mpqs_work_plan_report = qs.mpqs_work_plan_report
def qs_batch_cofactor_report = qs.qs_batch_cofactor_report
def qs_large_prime_filter_report = qs.qs_large_prime_filter_report
def qs_multiplier_report = qs.qs_multiplier_report
def qs_relation_filter_report = qs.qs_relation_filter_report
def quadratic_sieve_factor = qs.quadratic_sieve_factor
def quadratic_sieve_factor_report = qs.quadratic_sieve_factor_report
def siqs_cutoff_tune_report = qs.siqs_cutoff_tune_report
def siqs_factor = qs.siqs_factor
def siqs_factor_report = qs.siqs_factor_report
def siqs_polynomial_report = qs.siqs_polynomial_report
def siqs_relation_report = qs.siqs_relation_report

fn require_factor_pair(any factors, any a, any b, str label) any {
   assert(factors != nil, label + " returned nil")
   assert(
      (factors.get(0, 0) == a && factors.get(1, 0) == b) ||
      (factors.get(0, 0) == b && factors.get(1, 0) == a),
      label + " exact factors"
   )
}

#main {
   def qs = quadratic_sieve_factor_report(8051, 31, 256, 10)
   assert(dict_has(qs, "relation_collection_elapsed_ms"), "qs elapsed")
   assert(dict_has(qs, "candidate_count"), "qs candidates")
   assert(dict_has(qs, "smooth_tests"), "qs smooth tests")
   assert(dict_has(qs, "smooth_hits"), "qs smooth hits")
   assert(dict_has(qs, "smooth_misses"), "qs smooth misses")
   assert(int(qs.get("smooth_misses", -1)) == int(qs.get("smooth_tests", 0)) - int(qs.get("smooth_hits", 0)), "qs smooth miss accounting")
   def rel = siqs_relation_report(8051, 31, 3, 64, 8, false)
   assert(dict_has(rel, "relation_collection_elapsed_ms"), "siqs rel elapsed")
   assert(dict_has(rel, "smooth_tests"), "siqs rel smooth tests")
   assert(dict_has(rel, "smooth_hits"), "siqs rel smooth hits")
   def siqs = siqs_factor_report(8051, 31, 3, 64, 8, false)
   assert(siqs.get("method") == "self-initializing-quadratic-sieve", "siqs method")
   assert(dict_has(siqs, "relation_report"), "siqs relation report")
   assert(dict_has(siqs, "dependency_solve"), "siqs dependency solve")
   assert(dict_has(siqs, "elapsed_ms"), "siqs elapsed")
   def mpqs = mpqs_factor_report(8051, 31, 3, 128, 16)
   assert(mpqs.get("method") == "multi-window-quadratic-sieve", "mpqs method")
   assert(dict_has(mpqs, "selected_multiplier"), "mpqs multiplier")
   assert(dict_has(mpqs, "primary_attempt"), "mpqs primary attempt")
   assert(dict_has(mpqs.get("primary_attempt"), "window_reports"), "mpqs window reports")
   assert(dict_has(mpqs, "linear_algebra_relation_count"), "mpqs la rel count")
   assert(dict_has(mpqs, "elapsed_ms"), "mpqs elapsed")
   require_factor_pair(dixon_factor(8051, 128, 5000), 83, 97, "dixon")
   require_factor_pair(euler_factor(65, 64), 5, 13, "euler")
   def ns = gf2.gf2_nullspace_report([[1, 1, 0], [0, 1, 1]])
   assert(ns.get("rank", 0) == 2, "gf2 nullspace rank")
   assert(ns.get("nullity", 0) == 1, "gf2 nullspace nullity")
   assert(gf2.gf2_nullspace([[1, 1, 0], [0, 1, 1]]).len == 1, "gf2 nullspace compact api")
   def mat_A = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1]]
   def test_v = [0, 1, 0]
   def packed_mv = gf2.packed_gf2_normal_matvec_report(mat_A, test_v, 3)
   def sparse_mv = gf2.sparse_gf2_normal_matvec_report([[0], [1], [2], [0, 1, 2]], test_v, 3)
   assert(packed_mv.get("result", []) == [1, 0, 1], "packed normal matvec parity")
   assert(sparse_mv.get("result", []) == [1, 0, 1], "sparse normal matvec parity")
   def lanczos = gf2.block_lanczos_gf2_report([[1, 1, 0], [0, 1, 1]], 3, 4, 0, true)
   assert(lanczos.get("method", "") == "block-lanczos-gf2", "lanczos method")
   assert(lanczos.get("verified_count", 0) >= 1, "lanczos finds dependency")
   def precond = gf2.gf2_matrix_precondition_report([[1, 1, 0, 0], [0, 1, 1, 0], [0, 0, 0, 1], [0, 0, 0, 0]], 4)
   assert(precond.get("singleton_columns_removed", 0) == 1, "precondition singleton column")
   assert(precond.get("empty_rows_removed", 0) == 2, "precondition empty rows")
   def pipe = gf2.gf2_dependency_pipeline_report([[1, 1, 0], [0, 1, 1]], 3, "lanczos", true, 8, 0, true, 512)
   assert(pipe.get("method", "") == "gf2-dependency-pipeline", "pipeline method")
   assert(pipe.get("verified_count", 0) >= 1, "pipeline verified dependencies")
   def wiedemann = gf2.block_wiedemann_gf2_report([[1, 1, 0], [0, 1, 1]], 3, 8, 0, true)
   assert(wiedemann.get("method", "") == "block-wiedemann-gf2", "wiedemann method")
   assert(wiedemann.get("verified_count", 0) >= 1, "wiedemann finds dependency")
   def poly = siqs_polynomial_report(8051, 31, 3, 64)
   assert(poly.get("method") == "siqs-polynomial-generation", "siqs polynomial method")
   assert(dict_has(poly, "multiplier_report"), "siqs poly multiplier")
   assert(dict_has(poly, "polynomials"), "siqs poly polynomials")
   def plan = mpqs_work_plan_report(8051)
   assert(plan.get("method") == "mpqs-work-plan", "mpqs work plan method")
   assert(dict_has(plan, "requested_max_relations"), "mpqs work plan max rels")
   print("✓ std.math.crypto.factorization.classical self-test passed")
}
