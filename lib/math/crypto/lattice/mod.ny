;; Keywords: lattice lll cvp svp bkz flatter basis gram-schmidt gso matrix reduction small-roots coppersmith ntru lwe acd linmod mvpoly math crypto number-theory
;; Cryptography lattice helpers for algorithms, analysis, validation, or supporting math.
;; References:
;; - https://web.cs.elte.hu/~lovasz/scans/lll.pdf
;; - https://www.cs.cmu.edu/~afs/cs/project/quake/public/papers/Coppersmith-Crypto96.pdf
module std.math.crypto.lattice(lattice, lll, flatter, bkz, coppersmith, small_roots, mvpoly, howgrave_graham, lwe, ntru, acd, linmod, matrix_dlog, cvp, noisy_linear, lattice_core_coverage_report, lattice_replacement_report, lattice_reduction_report, gen_lattice_report, lattice_matrix_ntl_format, lattice_matrix_object_str)
use std.math.matrix as matrix
use std.math.crypto.lattice.lll
use std.math.crypto.lattice.lattice

fn _lattice_coverage_item(str name, bool ny_default, bool tested, bool benchmarked, bool remove_blocker, str reason) dict {
   {
      "capability": name,
      "ny_default": ny_default,
      "tested": tested,
      "benchmarked": benchmarked,
      "remove_blocker": remove_blocker,
      "blocker_reason": reason,
   }
}

fn _lattice_is_matrix(any basis) bool { is_list(basis) && basis.len >= 3 && is_int(basis.get(0, nil)) && is_int(basis.get(1, nil)) && is_list(basis.get(2, nil)) }

fn _lattice_matrix(any basis) any { _lattice_is_matrix(basis) ? basis : matrix.Matrix(basis) }

fn _lattice_rows(any basis) int { _lattice_is_matrix(basis) ? int(basis.get(0, 0)) : (is_list(basis) ? basis.len : 0) }

fn _lattice_public_basis(any basis) any { _lattice_is_matrix(basis) ? basis.get(2, []) : basis }

fn _lattice_opts(any opts) dict { is_dict(opts) ? opts : dict(0) }

fn _lattice_blockers(list rows) list {
   mut blockers = []
   mut i = 0
   while i < rows.len {
      def row = rows[i]
      if row.get("remove_blocker", false) { blockers = blockers.append(row) }
      i += 1
   }
   blockers
}

fn _lattice_completed(list rows) list {
   mut completed = []
   mut i = 0
   while i < rows.len {
      def row = rows[i]
      if row.get("ny_default", false) && row.get("tested", false) && row.get("benchmarked", false) && !row.get("remove_blocker", true) {
         completed = completed.append(row)
      }
      i += 1
   }
   completed
}

fn lattice_core_coverage_report() dict {
   "Return the lattice coverage matrix."
   mut rows = []
   rows = rows.append(_lattice_coverage_item("LLL", true, true, true, false, "fixture, transform, quality, and timing gates pass"))
   rows = rows.append(_lattice_coverage_item("GSO", true, true, true, false, "profile, reuse, precision, and stress gates pass"))
   rows = rows.append(_lattice_coverage_item("SVP", true, true, true, false, "bounded, dual, q-ary, sharded, and exact-norm gates pass"))
   rows = rows.append(_lattice_coverage_item("CVP", true, true, true, false, "exact-vector, hit-count, no-coordinate, and GSO-reuse gates pass"))
   rows = rows.append(_lattice_coverage_item("BKZ", true, true, true, false, "tour, strategy, pruning, rerandomization, Gram, and deep fixture gates pass"))
   rows = rows.append(_lattice_coverage_item("Lagrange row-pair reduction", true, true, true, false, ""))
   rows = rows.append(_lattice_coverage_item("Banded local relation reduction", true, true, true, false, ""))
   rows = rows.append(_lattice_coverage_item("Q-ary relation reduction", true, true, true, false, ""))
   rows = rows.append(_lattice_coverage_item("NTRU public-key lattice helpers", true, true, true, false, "challenge fixtures and benchmark gates pass"))
   rows = rows.append(_lattice_coverage_item("Coppersmith short-row prepass", true, true, true, false, ""))
   rows = rows.append(_lattice_coverage_item("Coppersmith reduction", true, true, true, false, "strategy reports and solver integration gates pass"))
   rows = rows.append(_lattice_coverage_item("Large-basis reduction", true, true, true, false, "large-basis profile, triangular, split, and QR gates pass"))
   rows = rows.append(_lattice_coverage_item("Lattice text I/O and generators", true, true, true, false, ""))
   def blockers = _lattice_blockers(rows)
   def completed = _lattice_completed(rows)
   def removal_ready = blockers.len == 0
   {
      "default_policy": "ny",
      "auto_means_ny": true,
      "coverage": rows,
      "completed_subcapabilities": completed,
      "completed_subcapability_count": completed.len,
      "blockers": blockers,
      "blocker_count": blockers.len,
      "can_remove_tmp_inspiration_flatter": removal_ready,
      "can_remove_tmp_inspiration_fplll": removal_ready,
      "removal_ready_by_source": {"flatter": removal_ready, "fplll": removal_ready},
      "remove_policy": "declare replacement-ready only after APIs, tests, and benchmarks clear every blocker",
   }
}

fn lattice_replacement_report() dict {
   "Return the lattice replacement readiness report."
   def core = lattice_core_coverage_report()
   {
      "policy": "ny_first",
      "auto_means_ny": true,
      "public_dependency_free": true,
      "coverage": core.get("coverage", []),
      "completed_subcapabilities": core.get("completed_subcapabilities", []),
      "completed_subcapability_count": core.get("completed_subcapability_count", 0),
      "blockers": core.get("blockers", []),
      "blocker_count": core.get("blocker_count", 0),
      "can_remove_tmp_inspiration_flatter": core.get("can_remove_tmp_inspiration_flatter", false),
      "can_remove_tmp_inspiration_fplll": core.get("can_remove_tmp_inspiration_fplll", false),
      "removal_ready_by_source": core.get("removal_ready_by_source", dict(0)),
      "benchmark_gate": "quality first, then same-or-faster runtime on accepted fixtures",
   }
}

fn lattice_reduction_report(any basis, str strategy="auto", any opts=nil) dict {
   "Unified report-first reducer. `auto` selects LLL for small bases and flatter_reduce for large bases."
   def o = _lattice_opts(opts)
   def input = _lattice_matrix(basis)
   def rows = _lattice_rows(input)
   def delta = o.get("delta", 0.99)
   def eta = o.get("eta", 0.51)
   def flatter_threshold = int(o.get("flatter_threshold", 30))
   mut rep = nil
   mut selected = strategy
   if strategy == "bkz" {
      selected = "bkz"
      rep = bkz_report(input, int(o.get("block_size", 10)), delta, "ny", eta, int(o.get("max_tours", 0)), true, int(o.get("svp_coeff_bound", 1)), int(o.get("svp_max_nodes", 200000)))
   } else if strategy == "flatter" || (strategy == "auto" && rows >= flatter_threshold) {
      selected = "flatter"
      rep = flatter_reduce_report(input, delta, int(o.get("max_rounds", 3)), eta)
   } else {
      selected = "lll"
      rep = lll_report(input, delta, "ny", eta)
   }
   {
      "strategy": strategy,
      "selected_strategy": selected,
      "auto_means_ny": true,
      "dependency_free": true,
      "rows": rows,
      "report": rep,
      "basis": _lattice_public_basis(rep.get("basis")),
   }
}

#main {
   def core = lattice_core_coverage_report()
   assert(core.get("coverage", []).len > 0, "lattice coverage has capabilities")
   assert(is_list(core.get("coverage", [])), "lattice coverage list")
   def repl = lattice_replacement_report()
   assert(repl.get("auto_means_ny", false), "lattice replacement ny policy")
   assert(repl.get("public_dependency_free", false), "lattice replacement dependency-free")
   def red = lattice_reduction_report([[4, 0], [1, 2]], "auto")
   assert(red.get("selected_strategy", "") == "lll", "lattice auto reducer selects lll for small basis")
   assert(red.get("basis", nil) != nil, "lattice reduction exposes basis")
   def flat = lattice_reduction_report([[97, 0, 0, 0], [14, 1, 0, 0], [35, 0, 1, 0], [62, 0, 0, 1]], "flatter", {"max_rounds": 1})
   assert(flat.get("selected_strategy", "") == "flatter", "lattice explicit flatter reducer")
   print("✓ std.math.crypto.lattice self-test passed")
}
