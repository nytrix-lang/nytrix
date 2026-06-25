;; Keywords: factorization classical gf2 linear-algebra nullspace lanczos wiedemann sparse packed
;; References: std.math.crypto.factorization.classical
module std.math.crypto.factorization.classical.gf2(_gf2_append_all, _gf2_block_krylov_candidates, _gf2_block_krylov_core_fields, _gf2_block_krylov_image_report, _gf2_block_krylov_iteration_plan, _gf2_block_krylov_op_fields, _gf2_block_krylov_op_fields_from, _gf2_block_krylov_post_lanczos_fields, _gf2_block_krylov_report, _gf2_block_krylov_report_fields, _gf2_block_krylov_setup, _gf2_block_krylov_solution_fields, _gf2_block_krylov_solution_report, _gf2_block_krylov_sparse_report, _gf2_block_krylov_sparse_setup, _gf2_block_krylov_walk_report, _gf2_columns_to_packed_rows, _gf2_columns_to_rows, _gf2_copy_packed_rows, _gf2_copy_vec, _gf2_copy_words, _gf2_cycle_to_dependency, _gf2_dense_vec_packed_words, _gf2_dense_width, _gf2_dependency_candidates, _gf2_dependency_sparse_pipeline_report, _gf2_expand_precondition_dependency, _gf2_independent_basis_report, _gf2_pack_dense_row, _gf2_pack_rows, _gf2_pack_sparse_row, _gf2_packed_basis_from_pivots, _gf2_packed_bit, _gf2_packed_dot_parity, _gf2_packed_eliminate_pivot, _gf2_packed_eliminate_rows, _gf2_packed_find_pivot, _gf2_packed_is_zero, _gf2_packed_matvec_is_zero, _gf2_packed_matvec_words, _gf2_packed_nonzero_rows, _gf2_packed_rows_nullspace_report, _gf2_packed_set_bit, _gf2_packed_swap_rank, _gf2_packed_to_dense, _gf2_packed_transpose_matvec_words, _gf2_packed_transpose_matvec_words_only, _gf2_packed_width, _gf2_packed_words_equal, _gf2_packed_words_hash, _gf2_packed_xor_inplace, _gf2_pipeline_empty_linear_algebra, _gf2_pipeline_exact_closure, _gf2_pipeline_expand_candidates, _gf2_pipeline_fields, _gf2_pipeline_immediate_linear_algebra, _gf2_pipeline_linear_algebra, _gf2_pipeline_precondition, _gf2_pipeline_verify, _gf2_post_lanczos_prefix_rows, _gf2_precondition_active_cols2, _gf2_precondition_active_rows2, _gf2_precondition_clique_pass, _gf2_precondition_decrement_intersection_counts, _gf2_precondition_find_other_col2, _gf2_precondition_initial_columns, _gf2_precondition_nochange_state, _gf2_precondition_prune_pass, _gf2_precondition_quick_noop, _gf2_precondition_reduced_state, _gf2_precondition_row_counts2, _gf2_precondition_trim_heavy, _gf2_seed_vec, _gf2_seen_packed_words_add, _gf2_sparse_back_eliminate, _gf2_sparse_basis_from_pivots, _gf2_sparse_contains, _gf2_sparse_elim_state, _gf2_sparse_eliminate_row, _gf2_sparse_eliminate_rows, _gf2_sparse_entry_count, _gf2_sparse_from_dense_rows, _gf2_sparse_from_dense_rows_until, _gf2_sparse_insert, _gf2_sparse_insert_pivot, _gf2_sparse_is_clean, _gf2_sparse_matvec, _gf2_sparse_matvec_into, _gf2_sparse_matvec_is_zero, _gf2_sparse_matvec_vec, _gf2_sparse_normalize, _gf2_sparse_remove, _gf2_sparse_tail_rows_report, _gf2_sparse_to_dense, _gf2_sparse_toggle, _gf2_sparse_transpose_matvec, _gf2_sparse_transpose_matvec_into, _gf2_sparse_width, _gf2_sparse_xor, _gf2_vec_is_zero, _gf2_vec_pivot, _gf2_vec_weight, _gf2_verified_basis_report, _gf2_verified_sparse_basis_report, _gf2_word_bits, _gf2_word_count, _gf2_xor_vec, _gf2_xor_vec_into, _gf2_zero_vec, _gf2_zero_words, block_lanczos_gf2_nullspace, block_lanczos_gf2_report, block_wiedemann_gf2_nullspace, block_wiedemann_gf2_report, gf2_dependency_candidates, gf2_dependency_candidates_report, gf2_dependency_pipeline_report, gf2_matrix_precondition_report, gf2_nullspace, gf2_nullspace_report, packed_gf2_matvec_report, packed_gf2_normal_matvec_report, packed_gf2_nullspace, packed_gf2_nullspace_report, sparse_gf2_matvec_report, sparse_gf2_normal_matvec_report, sparse_gf2_nullspace, sparse_gf2_nullspace_report)
use std.math.nt
use std.math.scalar as math
use std.math.matrix as matrix
use std.math.bin (bit_count)
use std.os (ticks)
use std.math.crypto.factorization.classical.misc

fn _gf2_sparse_contains(list row, int col) bool {
   mut i = 0
   while i < row.len {
      if int(row[i]) == col { return true }
      i += 1
   }
   false
}

fn _gf2_sparse_remove(list row, int col) list {
   mut out = []
   mut i = 0
   while i < row.len {
      def v = int(row[i])
      if v != col { out = out.append(v) }
      i += 1
   }
   out
}

fn _gf2_sparse_insert(list row, int col) list {
   mut out = []
   mut inserted = false
   mut i = 0
   while i < row.len {
      def v = int(row[i])
      if v == col { inserted = true }
      if !inserted && col < v {
         out = out.append(col)
         inserted = true
      }
      out = out.append(v)
      i += 1
   }
   if !inserted { out = out.append(col) }
   out
}

fn _gf2_sparse_toggle(list row, int col) list {
   _gf2_sparse_contains(row, col) ? _gf2_sparse_remove(row, col) : _gf2_sparse_insert(row, col)
}

fn _gf2_sparse_is_clean(list row, int width) bool {
   mut prev = -1
   mut i = 0
   while i < row.len {
      def c = int(row[i])
      if c < 0 || (width > 0 && c >= width) || c <= prev { return false }
      prev = c
      i += 1
   }
   true
}

fn _gf2_sparse_normalize(list row, int width) list {
   if _gf2_sparse_is_clean(row, width) { return row }
   mut out = []
   mut i = 0
   while i < row.len {
      def c = int(row[i])
      if c >= 0 && (width <= 0 || c < width) { out = _gf2_sparse_toggle(out, c) }
      i += 1
   }
   out
}

fn _gf2_sparse_xor(list a, list b) list {
   mut out = list(a.len + b.len)
   mut i, j, oi = 0, 0, 0
   while i < a.len && j < b.len {
      def av = int(a[i])
      def bv = int(b[j])
      if av == bv {
         i += 1
         j += 1
      } else if av < bv {
         out[oi] = av
         oi += 1
         i += 1
      } else {
         out[oi] = bv
         oi += 1
         j += 1
      }
   }
   while i < a.len {
      out[oi] = int(a[i])
      oi += 1
      i += 1
   }
   while j < b.len {
      out[oi] = int(b[j])
      oi += 1
      j += 1
   }
   __list_set_len(out, oi)
   out
}

fn _gf2_sparse_to_dense(list row, int width) list {
   mut out = []
   mut i = 0
   while i < width {
      out = out.append(_gf2_sparse_contains(row, i) ? 1 : 0)
      i += 1
   }
   out
}

fn _gf2_sparse_from_dense_rows(list rows) list {
   mut out = list(rows.len)
   mut i = 0
   while i < rows.len {
      def list<int> r = rows[i]
      mut sparse = []
      mut j = 0
      while j < r.len {
         if (r[j] & 1) == 1 { sparse = sparse.append(j) }
         j += 1
      }
      out[i] = sparse
      i += 1
   }
   out
}

fn _gf2_sparse_from_dense_rows_until(list rows, int max_entries, int profile_prefix=0, int width=0) dict {
   mut out = list(rows.len)
   mut work_rows = profile_prefix > 0 ? list(max(0, rows.len - profile_prefix)) : []
   mut entries = 0
   mut work_entries = 0
   mut work_word_terms = 0
   mut i = 0
   while i < rows.len {
      def list<int> r = rows[i]
      mut sparse = []
      mut j = 0
      mut row_entries = 0
      while j < r.len {
         if (r[j] & 1) == 1 {
            entries += 1
            row_entries += 1
            if max_entries >= 0 && entries > max_entries {
               return _dict_with(10, [["rows", []], ["work_rows", []], ["entries", entries], ["overflow", true], ["work_entries", work_entries], ["work_word_terms", work_word_terms]])
            }
            sparse = sparse.append(j)
         }
         j += 1
      }
      if i >= profile_prefix {
         work_entries += row_entries
         work_word_terms += row_entries
         if profile_prefix > 0 { work_rows[i - profile_prefix] = sparse }
      }
      out[i] = sparse
      i += 1
   }
   _dict_with(10, [["rows", out], ["work_rows", profile_prefix > 0 ? work_rows : out], ["entries", entries], ["overflow", false], ["work_entries", work_entries], ["work_word_terms", work_word_terms]])
}

fn _gf2_sparse_entry_count(list sparse_rows) int {
   mut total = 0
   mut i = 0
   while i < sparse_rows.len {
      total += sparse_rows[i].len
      i += 1
   }
   total
}

fn _gf2_sparse_tail_rows_report(list sparse_rows, int prefix) dict {
   def start = max(0, min(prefix, sparse_rows.len))
   def n = sparse_rows.len - start
   mut rows = list(n)
   mut entries = 0
   mut i = start
   while i < sparse_rows.len {
      def row = sparse_rows[i]
      rows[i - start] = row
      entries += row.len
      i += 1
   }
   _dict_with(4, [["rows", rows], ["entries", entries]])
}

fn _gf2_post_lanczos_prefix_rows(int row_count, int width, bool use_sparse_kernel) int {
   if !use_sparse_kernel || row_count < 128 || width <= 0 { return 0 }
   def target = max(8, _gf2_word_bits() * 2)
   if row_count <= target * 2 { return 0 }
   min(target, row_count / 3)
}

fn _gf2_sparse_matvec(list<list<int>> sparse_rows, list<int> vector) list {
   [_gf2_sparse_matvec_vec(sparse_rows, vector), _gf2_sparse_entry_count(sparse_rows)]
}

fn _gf2_sparse_matvec_vec(list<list<int>> sparse_rows, list<int> vector) list<int> {
   mut out = list(sparse_rows.len)
   _gf2_sparse_matvec_into(sparse_rows, vector, out)
}

fn _gf2_sparse_matvec_into(list<list<int>> sparse_rows, list<int> vector, list<int> out) list<int> {
   mut i = 0
   while i < sparse_rows.len {
      def list<int> row = sparse_rows[i]
      mut acc = 0
      mut j = 0
      while j < row.len {
         def col = row[j]
         acc = acc ^^ (vector[col] & 1)
         j += 1
      }
      out[i] = acc & 1
      i += 1
   }
   out
}

fn _gf2_sparse_matvec_is_zero(list<list<int>> sparse_rows, list<int> vector) bool {
   mut i = 0
   while i < sparse_rows.len {
      def list<int> row = sparse_rows[i]
      mut acc = 0
      mut j = 0
      while j < row.len {
         acc = acc ^^ (vector[row[j]] & 1)
         j += 1
      }
      if (acc & 1) != 0 { return false }
      i += 1
   }
   true
}

fn _gf2_sparse_transpose_matvec(list<list<int>> sparse_rows, list<int> y, int width) list {
   mut list<int> out = _gf2_zero_vec(width)
   mut row_xor_ops = 0
   mut entry_xor_ops = 0
   mut i = 0
   while i < sparse_rows.len {
      if (y[i] & 1) != 0 {
         def list<int> row = sparse_rows[i]
         mut j = 0
         while j < row.len {
            def col = row[j]
            out[col] = out[col] ^^ 1
            j += 1
         }
         entry_xor_ops += row.len
         row_xor_ops += 1
      }
      i += 1
   }
   [out, row_xor_ops, entry_xor_ops]
}

fn _gf2_sparse_transpose_matvec_into(list<list<int>> sparse_rows, list<int> y, list<int> out, list<int> counters) bool {
   mut row_xor_ops = 0
   mut entry_xor_ops = 0
   mut nonzero_count = 0
   mut i = 0
   while i < out.len {
      out[i] = 0
      i += 1
   }
   i = 0
   while i < sparse_rows.len {
      if (y[i] & 1) != 0 {
         def list<int> row = sparse_rows[i]
         mut j = 0
         while j < row.len {
            def col = row[j]
            def next = out[col] ^^ 1
            out[col] = next
            nonzero_count += next == 1 ? 1 : -1
            j += 1
         }
         entry_xor_ops += row.len
         row_xor_ops += 1
      }
      i += 1
   }
   counters[0] = counters[0] + row_xor_ops
   counters[1] = counters[1] + entry_xor_ops
   nonzero_count != 0
}

fn sparse_gf2_matvec_report(list sparse_rows, list vector, int width=0) dict {
   "Return A*v over GF(2) from sparse row indices."
   def t0 = ticks()
   mut w = width
   if w <= 0 { w = vector.len }
   mut clean = []
   mut i = 0
   while i < sparse_rows.len {
      clean = clean.append(_gf2_sparse_normalize(sparse_rows.get(i), w))
      i += 1
   }
   def mv = _gf2_sparse_matvec(clean, vector)
   _report_with("sparse-gf2-matvec", t0, [
         ["rows", clean.len], ["cols", w],
         ["nonzeros", _gf2_sparse_entry_count(clean)],
         ["result", mv.get(0)], ["entry_ops", mv.get(1, 0)],
      ])
}

fn sparse_gf2_normal_matvec_report(list sparse_rows, list vector, int width=0) dict {
   "Return A^T*A*v over GF(2) from sparse row indices."
   def t0 = ticks()
   mut w = width
   if w <= 0 { w = vector.len }
   mut clean = []
   mut i = 0
   while i < sparse_rows.len {
      clean = clean.append(_gf2_sparse_normalize(sparse_rows.get(i), w))
      i += 1
   }
   def mv, tv = _gf2_sparse_matvec(clean, vector), _gf2_sparse_transpose_matvec(clean, mv.get(0), w)
   _report_with("sparse-gf2-normal-matvec", t0, [
         ["rows", clean.len], ["cols", w],
         ["nonzeros", _gf2_sparse_entry_count(clean)],
         ["intermediate", mv.get(0)], ["result", tv.get(0)],
         ["entry_dot_ops", mv.get(1, 0)],
         ["transpose_row_xor_ops", tv.get(1, 0)],
         ["transpose_entry_xor_ops", tv.get(2, 0)],
      ])
}

fn _gf2_sparse_width(list sparse_rows, int width) int {
   if width > 0 { return width }
   mut max_col = -1
   mut i = 0
   while i < sparse_rows.len {
      def r = sparse_rows[i]
      mut j = 0
      while j < r.len {
         def col = int(r[j])
         if col > max_col { max_col = col }
         j += 1
      }
      i += 1
   }
   max_col + 1
}

fn _gf2_sparse_elim_state() dict {
   _dict_with(8, [
         ["pivots", []], ["pivot_rows", []], ["pivot_index", dict()],
         ["pivot_lookup_hits", 0], ["pivot_lookup_misses", 0],
         ["row_xor_ops", 0], ["back_eliminate_ops", 0],
      ])
}

fn _gf2_sparse_back_eliminate(list pivot_rows_in, list row, int pivot) dict {
   mut pivot_rows = pivot_rows_in
   mut ops = 0
   mut k = 0
   while k < pivot_rows.len {
      if _gf2_sparse_contains(pivot_rows.get(k), pivot) {
         pivot_rows[k] = _gf2_sparse_xor(pivot_rows.get(k), row)
         ops += 1
      }
      k += 1
   }
   _dict_with(4, [["pivot_rows", pivot_rows], ["ops", ops]])
}

fn _gf2_sparse_insert_pivot(dict state, list row, int pivot, str pkey) dict {
   def back = _gf2_sparse_back_eliminate(state.get("pivot_rows", []), row, pivot)
   def pivots = state.get("pivots", []).append(pivot)
   def pivot_rows = back.get("pivot_rows", []).append(row)
   def pivot_index = state.get("pivot_index", dict()).set(pkey, pivots.len - 1)
   _set_fields(state, [
         ["pivots", pivots],
         ["pivot_rows", pivot_rows],
         ["pivot_index", pivot_index],
         ["pivot_lookup_misses", int(state.get("pivot_lookup_misses", 0)) + 1],
         ["back_eliminate_ops", int(state.get("back_eliminate_ops", 0)) + int(back.get("ops", 0))],
      ])
}

fn _gf2_sparse_eliminate_row(dict state_in, list sparse_row, int width) dict {
   mut state = state_in
   mut row = _gf2_sparse_normalize(sparse_row, width)
   while row.len > 0 {
      def pivot = int(row.get(0))
      def pkey = to_str(pivot)
      def idx = int(state.get("pivot_index", dict()).get(pkey, -1))
      if idx >= 0 {
         row = _gf2_sparse_xor(row, state.get("pivot_rows", []).get(idx))
         state = state.set("pivot_lookup_hits", int(state.get("pivot_lookup_hits", 0)) + 1).set("row_xor_ops", int(state.get("row_xor_ops", 0)) + 1)
      } else {
         state = _gf2_sparse_insert_pivot(state, row, pivot, pkey)
         row = []
      }
   }
   state
}

fn _gf2_sparse_eliminate_rows(list sparse_rows, int width) dict {
   mut state = _gf2_sparse_elim_state()
   mut i = 0
   while i < sparse_rows.len {
      state = _gf2_sparse_eliminate_row(state, sparse_rows.get(i), width)
      i += 1
   }
   state
}

fn _gf2_sparse_basis_from_pivots(list pivots, list pivot_rows, dict pivot_index, int width) list {
   mut basis = []
   mut col = 0
   while col < width {
      if !pivot_index.contains(to_str(col)) {
         mut sparse_v = [col]
         mut i = 0
         while i < pivots.len {
            if _gf2_sparse_contains(pivot_rows.get(i), col) { sparse_v = _gf2_sparse_insert(sparse_v, int(pivots.get(i))) }
            i += 1
         }
         basis = basis.append(_gf2_sparse_to_dense(sparse_v, width))
      }
      col += 1
   }
   basis
}

fn sparse_gf2_nullspace_report(list sparse_rows, int width=0) dict {
   "Return right-nullspace diagnostics for a sparse GF(2) matrix."
   def t0 = ticks()
   def w = _gf2_sparse_width(sparse_rows, width)
   def eliminated = _gf2_sparse_eliminate_rows(sparse_rows, w)
   def pivots = eliminated.get("pivots", [])
   def pivot_rows = eliminated.get("pivot_rows", [])
   def basis = _gf2_sparse_basis_from_pivots(pivots, pivot_rows, eliminated.get("pivot_index", dict()), w)
   _report_with("sparse-gf2-elimination", t0, [
         ["rows", sparse_rows.len], ["cols", w],
         ["rank", pivots.len], ["nullity", basis.len],
         ["pivots", pivots], ["pivot_rows_sparse", pivot_rows],
         ["basis", basis],
         ["pivot_lookup_hits", eliminated.get("pivot_lookup_hits", 0)],
         ["pivot_lookup_misses", eliminated.get("pivot_lookup_misses", 0)],
         ["row_xor_ops", eliminated.get("row_xor_ops", 0)],
         ["back_eliminate_ops", eliminated.get("back_eliminate_ops", 0)],
      ])
}

fn sparse_gf2_nullspace(list sparse_rows, int width=0) list {
   "Return a dense basis for a sparse GF(2) right-nullspace."
   sparse_gf2_nullspace_report(sparse_rows, width).get("basis", [])
}

fn gf2_nullspace_report(list rows) dict {
   "Return GF(2) nullspace/rank diagnostics for a binary matrix."
   def t0 = ticks()
   def nr = rows.len
   def nc = nr == 0 ? 0 : rows.get(0).len
   mut clean = []
   mut i = 0
   while i < nr {
      def r = rows.get(i)
      if r.len != nc { panic("gf2_nullspace_report: ragged matrix") }
      mut row = []
      mut j = 0
      while j < nc {
         row = row.append(int(r.get(j)) & 1)
         j += 1
      }
      clean = clean.append(row)
      i += 1
   }
   def M = matrix.Matrix(clean)
   def rr = matrix.matrix_rref_mod(M, 2)
   def basis = matrix.matrix_nullspace_mod(M, 2)
   _report_with("gf2-rref-nullspace", t0, [
         ["rows", nr], ["cols", nc],
         ["rank", rr.get(1).len], ["nullity", basis.len],
         ["pivots", rr.get(1)], ["basis", basis],
      ])
}

fn gf2_nullspace(list rows) list {
   "Return a basis for the GF(2) right-nullspace of `rows`."
   gf2_nullspace_report(rows).get("basis", [])
}

fn _gf2_word_bits() int { 30 }

fn _gf2_word_count(int width) int {
   width <= 0 ? 0 : ((width + _gf2_word_bits() - 1) / _gf2_word_bits())
}

fn _gf2_zero_words(int words) list {
   mut out = list(words)
   mut i = 0
   while i < words {
      out[i] = 0
      i += 1
   }
   out
}

fn _gf2_pack_dense_row(list row, int width) list {
   def wb = _gf2_word_bits()
   mut out = _gf2_zero_words(_gf2_word_count(width))
   mut i = 0
   def limit = min(width, row.len)
   while i < limit {
      if (int(row[i]) & 1) != 0 {
         def wi, bi = i / wb, i % wb
         out[wi] = int(out[wi]) | (1 << bi)
      }
      i += 1
   }
   out
}

fn _gf2_dense_vec_packed_words(list row, int width) list {
   def words = _gf2_pack_dense_row(row, width)
   [!_gf2_packed_is_zero(words), words.len, words]
}

fn _gf2_packed_words_hash(list words) int {
   mut h = 2166136261
   mut i = 0
   while i < words.len {
      h = (h * 16777619 + int(words[i]) + i) % 2147483647
      if h < 0 { h = 0 - h }
      i += 1
   }
   h
}

fn _gf2_packed_words_equal(list a, list b) bool {
   if a.len != b.len { return false }
   mut i = 0
   while i < a.len {
      if int(a[i]) != int(b[i]) { return false }
      i += 1
   }
   true
}

fn _gf2_seen_packed_words_add(dict seen, list words) list {
   def key = _gf2_packed_words_hash(words)
   mut bucket = seen.get(key, [])
   mut i = 0
   while i < bucket.len {
      if _gf2_packed_words_equal(bucket[i], words) { return [true, seen] }
      i += 1
   }
   bucket = bucket.append(words)
   [false, seen.set(key, bucket)]
}

fn _gf2_pack_sparse_row(list row, int width) list {
   def wb = _gf2_word_bits()
   mut out = _gf2_zero_words(_gf2_word_count(width))
   mut i = 0
   while i < row.len {
      def col = int(row.get(i, -1))
      if col >= 0 && col < width {
         def wi, bi = col / wb, col % wb
         out[wi] = int(out[wi]) ^^ (1 << bi)
      }
      i += 1
   }
   out
}

fn _gf2_packed_bit(list row, int col) int {
   if col < 0 { return 0 }
   def wb, wi = _gf2_word_bits(), col / _gf2_word_bits()
   if wi < 0 || wi >= row.len { return 0 }
   (int(row[wi]) >> (col % wb)) & 1
}

fn _gf2_packed_xor_inplace(list a, list b, int start_word=0) list {
   mut int i = start_word < 0 ? 0 : start_word
   def int n = min(a.len, b.len)
   while i < n {
      a[i] = int(a[i]) ^^ int(b[i])
      i += 1
   }
   a
}

fn _gf2_packed_is_zero(list row) bool {
   mut i = 0
   while i < row.len {
      if int(row[i]) != 0 { return false }
      i += 1
   }
   true
}

fn _gf2_packed_to_dense(list row, int width) list {
   mut out = list(width)
   mut i = 0
   while i < width {
      out[i] = _gf2_packed_bit(row, i)
      i += 1
   }
   out
}

fn _gf2_packed_set_bit(list row, int col) list {
   if col < 0 { return row }
   def wb, wi = _gf2_word_bits(), col / _gf2_word_bits()
   if wi >= 0 && wi < row.len { row[wi] = int(row[wi]) ^^ (1 << (col % wb)) }
   row
}

fn _gf2_copy_words(list row) list {
   mut out = list(row.len)
   mut i = 0
   while i < row.len {
      out[i] = int(row[i])
      i += 1
   }
   out
}

fn _gf2_copy_vec(list<int> row) list<int> {
   mut out = list(row.len)
   mut i = 0
   while i < row.len {
      out[i] = row[i]
      i += 1
   }
   out
}

fn _gf2_pack_rows(list rows, int width=0, bool sparse=false) list {
   mut nc = width
   if nc <= 0 && rows.len > 0 {
      if sparse {
         mut max_col = -1
         mut i = 0
         while i < rows.len {
            def r = rows.get(i)
            mut j = 0
            while j < r.len {
               if int(r.get(j, -1)) > max_col { max_col = int(r.get(j, -1)) }
               j += 1
            }
            i += 1
         }
         nc = max_col + 1
      } else {
         nc = rows.get(0).len
      }
   }
   mut packed = list(rows.len)
   mut i = 0
   while i < rows.len {
      def r = rows.get(i)
      packed[i] = sparse ? _gf2_pack_sparse_row(r, nc) : _gf2_pack_dense_row(r, nc)
      i += 1
   }
   [packed, nc]
}

fn _gf2_packed_dot_parity(list row_words, list vec_words) int {
   mut parity = 0
   mut i = 0
   def limit = min(row_words.len, vec_words.len)
   while i < limit {
      parity = parity ^^ (bit_count(row_words[i] & vec_words[i]) & 1)
      i += 1
   }
   parity & 1
}

fn _gf2_packed_matvec_words(list packed_rows, list vec_words) list {
   mut out = list(packed_rows.len)
   mut word_and_ops = 0
   mut popcount_ops = 0
   mut i = 0
   while i < packed_rows.len {
      def row = packed_rows[i]
      out[i] = _gf2_packed_dot_parity(row, vec_words)
      word_and_ops += min(row.len, vec_words.len)
      popcount_ops += min(row.len, vec_words.len)
      i += 1
   }
   [out, word_and_ops, popcount_ops]
}

fn _gf2_packed_matvec_is_zero(list packed_rows, list vec_words) list {
   mut word_and_ops = 0
   mut popcount_ops = 0
   mut i = 0
   while i < packed_rows.len {
      def row = packed_rows[i]
      mut parity = 0
      mut j = 0
      def limit = min(row.len, vec_words.len)
      while j < limit {
         parity = parity ^^ (bit_count(int(row[j]) & int(vec_words[j])) & 1)
         j += 1
      }
      word_and_ops += limit
      popcount_ops += limit
      if (parity & 1) != 0 { return [false, word_and_ops, popcount_ops] }
      i += 1
   }
   [true, word_and_ops, popcount_ops]
}

fn _gf2_packed_transpose_matvec_words(list packed_rows, list y, int width) list {
   mut out_words = _gf2_zero_words(_gf2_word_count(width))
   mut row_xor_ops = 0
   mut word_xor_ops = 0
   mut i = 0
   while i < packed_rows.len {
      if (int(y[i]) & 1) != 0 {
         def row = packed_rows[i]
         mut w = 0
         def limit = min(out_words.len, row.len)
         while w < limit {
            out_words[w] = out_words[w] ^^ row[w]
            word_xor_ops += 1
            w += 1
         }
         row_xor_ops += 1
      }
      i += 1
   }
   [_gf2_packed_to_dense(out_words, width), row_xor_ops, word_xor_ops, out_words]
}

fn _gf2_packed_transpose_matvec_words_only(list packed_rows, list y, int width) list {
   mut out_words = _gf2_zero_words(_gf2_word_count(width))
   mut row_xor_ops = 0
   mut word_xor_ops = 0
   mut i = 0
   while i < packed_rows.len {
      if (int(y[i]) & 1) != 0 {
         def row = packed_rows[i]
         mut w = 0
         def limit = min(out_words.len, row.len)
         while w < limit {
            out_words[w] = out_words[w] ^^ row[w]
            word_xor_ops += 1
            w += 1
         }
         row_xor_ops += 1
      }
      i += 1
   }
   [row_xor_ops, word_xor_ops, out_words]
}

fn packed_gf2_matvec_report(list rows, list vector, int width=0, bool sparse=false) dict {
   "Return A*v over GF(2) using packed row words and report word/popcount counters."
   def t0 = ticks()
   def packed_info = _gf2_pack_rows(rows, width, sparse)
   def packed_rows = packed_info.get(0)
   def w = int(packed_info.get(1, width))
   def v_words = _gf2_pack_dense_row(vector, w)
   def mv = _gf2_packed_matvec_words(packed_rows, v_words)
   _report_with("packed-gf2-matvec", t0, [
         ["rows", rows.len], ["cols", w],
         ["word_bits", _gf2_word_bits()],
         ["word_count", _gf2_word_count(w)],
         ["sparse_input", sparse],
         ["result", mv.get(0)],
         ["word_and_ops", mv.get(1, 0)],
         ["popcount_ops", mv.get(2, 0)],
      ])
}

fn packed_gf2_normal_matvec_report(list rows, list vector, int width=0, bool sparse=false) dict {
   "Return A^T*A*v over GF(2) using packed row words."
   def t0 = ticks()
   def packed_info = _gf2_pack_rows(rows, width, sparse)
   def packed_rows = packed_info.get(0)
   def w = int(packed_info.get(1, width))
   def v_words = _gf2_pack_dense_row(vector, w)
   def mv = _gf2_packed_matvec_words(packed_rows, v_words)
   def tv = _gf2_packed_transpose_matvec_words(packed_rows, mv.get(0), w)
   _report_with("packed-gf2-normal-matvec", t0, [
         ["rows", rows.len], ["cols", w],
         ["word_bits", _gf2_word_bits()],
         ["word_count", _gf2_word_count(w)],
         ["sparse_input", sparse],
         ["intermediate", mv.get(0)], ["result", tv.get(0)],
         ["word_and_ops", mv.get(1, 0)],
         ["popcount_ops", mv.get(2, 0)],
         ["transpose_row_xor_ops", tv.get(1, 0)],
         ["transpose_word_xor_ops", tv.get(2, 0)],
      ])
}

fn _gf2_columns_to_packed_rows(list cols, int row_count) list {
   def width = cols.len
   def word_count = _gf2_word_count(width)
   def wb = _gf2_word_bits()
   mut rows = list(row_count)
   mut r = 0
   while r < row_count {
      mut words = _gf2_zero_words(word_count)
      mut c = 0
      while c < width {
         def list<int> col = cols[c]
         if (col[r] & 1) != 0 {
            def wi, bi = c / wb, c % wb
            words[wi] = int(words[wi]) | (1 << bi)
         }
         c += 1
      }
      rows[r] = words
      r += 1
   }
   rows
}

fn _gf2_packed_width(list packed_rows, int width) int {
   if width > 0 { return width }
   packed_rows.len > 0 ? packed_rows.get(0).len * _gf2_word_bits() : 0
}

fn _gf2_copy_packed_rows(list packed_rows) list {
   mut packed = []
   mut i = 0
   while i < packed_rows.len {
      packed = packed.append(_gf2_copy_words(packed_rows.get(i)))
      i += 1
   }
   packed
}

fn _gf2_packed_find_pivot(list packed, int rank, int col) list {
   mut pr = -1
   mut scans = 0
   mut rix = rank
   while rix < packed.len && pr < 0 {
      scans += 1
      if _gf2_packed_bit(packed.get(rix), col) != 0 { pr = rix }
      rix += 1
   }
   [pr, scans]
}

fn _gf2_packed_swap_rank(list packed_in, int rank, int pivot_row) dict {
   mut packed = packed_in
   if pivot_row != rank {
      def tmp = packed.get(rank)
      packed[rank] = packed.get(pivot_row)
      packed[pivot_row] = tmp
      return _dict_with(4, [["packed", packed], ["row_swaps", 1]])
   }
   _dict_with(4, [["packed", packed], ["row_swaps", 0]])
}

fn _gf2_packed_eliminate_pivot(list packed_in, int rank, int col, int width) dict {
   mut packed = packed_in
   def start_word = col / _gf2_word_bits()
   mut xor_ops = 0
   mut word_xor_ops = 0
   mut rix = 0
   while rix < packed.len {
      if rix != rank && _gf2_packed_bit(packed.get(rix), col) != 0 {
         packed[rix] = _gf2_packed_xor_inplace(packed.get(rix), packed.get(rank), start_word)
         xor_ops += 1
         word_xor_ops += _gf2_word_count(width) - start_word
      }
      rix += 1
   }
   _dict_with(6, [["packed", packed], ["xor_ops", xor_ops], ["word_xor_ops", word_xor_ops]])
}

fn _gf2_packed_eliminate_rows(list packed_rows, int width) dict {
   mut packed = _gf2_copy_packed_rows(packed_rows)
   mut rank = 0
   mut pivots = []
   mut pivot_cols = dict()
   mut row_swaps = 0
   mut xor_ops = 0
   mut pivot_scans = 0
   mut word_xor_ops = 0
   mut col = 0
   while col < width && rank < packed.len {
      def found = _gf2_packed_find_pivot(packed, rank, col)
      def pr = int(found.get(0))
      pivot_scans += int(found.get(1, 0))
      if pr >= 0 {
         def swapped = _gf2_packed_swap_rank(packed, rank, pr)
         packed = swapped.get("packed")
         row_swaps += int(swapped.get("row_swaps", 0))
         def eliminated = _gf2_packed_eliminate_pivot(packed, rank, col, width)
         packed = eliminated.get("packed")
         xor_ops += int(eliminated.get("xor_ops", 0))
         word_xor_ops += int(eliminated.get("word_xor_ops", 0))
         pivots = pivots.append(col)
         pivot_cols = pivot_cols.set(to_str(col), true)
         rank += 1
      }
      col += 1
   }
   _dict_with(14, [
         ["packed", packed], ["rank", rank], ["pivots", pivots],
         ["pivot_cols", pivot_cols], ["row_swaps", row_swaps],
         ["xor_ops", xor_ops], ["word_xor_ops", word_xor_ops],
         ["pivot_scans", pivot_scans],
      ])
}

fn _gf2_packed_basis_from_pivots(list packed, list pivots, dict pivot_cols, int width, int max_basis) dict {
   mut basis = []
   mut basis_packed = []
   mut col = 0
   while col < width && (max_basis <= 0 || basis.len < max_basis) {
      if !pivot_cols.contains(to_str(col)) {
         mut v, i = _gf2_packed_set_bit(_gf2_zero_words(_gf2_word_count(width)), col), 0
         while i < pivots.len {
            if _gf2_packed_bit(packed.get(i), col) != 0 { v = _gf2_packed_set_bit(v, int(pivots.get(i))) }
            i += 1
         }
         basis_packed = basis_packed.append(v)
         basis = basis.append(_gf2_packed_to_dense(v, width))
      }
      col += 1
   }
   _dict_with(4, [["basis", basis], ["basis_packed", basis_packed]])
}

fn _gf2_packed_nonzero_rows(list packed) int {
   mut nonzero_rows = 0
   mut i = 0
   while i < packed.len {
      if !_gf2_packed_is_zero(packed.get(i)) { nonzero_rows += 1 }
      i += 1
   }
   nonzero_rows
}

fn _gf2_packed_rows_nullspace_report(list packed_rows, int width=0, int max_basis=0, str method="packed-gf2-elimination", any started=nil) dict {
   mut t0 = started
   if t0 == nil { t0 = ticks() }
   def nc = _gf2_packed_width(packed_rows, width)
   def eliminated = _gf2_packed_eliminate_rows(packed_rows, nc)
   def packed = eliminated.get("packed", [])
   def rank = int(eliminated.get("rank", 0))
   def pivots = eliminated.get("pivots", [])
   def basis_report = _gf2_packed_basis_from_pivots(packed, pivots, eliminated.get("pivot_cols", dict()), nc, max_basis)
   def basis = basis_report.get("basis", [])
   _report_with(method, t0, [
         ["rows", packed_rows.len], ["cols", nc],
         ["word_bits", _gf2_word_bits()],
         ["word_count", _gf2_word_count(nc)],
         ["rank", rank], ["nonzero_rows", _gf2_packed_nonzero_rows(packed)],
         ["nullity", nc - rank],
         ["basis_count", basis.len],
         ["basis_limited", max_basis > 0 && basis.len < (nc - rank)],
         ["basis_limit", max_basis], ["pivots", pivots],
         ["row_swaps", eliminated.get("row_swaps", 0)],
         ["xor_ops", eliminated.get("xor_ops", 0)],
         ["word_xor_ops", eliminated.get("word_xor_ops", 0)],
         ["pivot_scans", eliminated.get("pivot_scans", 0)],
         ["basis", basis], ["basis_packed", basis_report.get("basis_packed", [])],
      ])
}

fn packed_gf2_nullspace_report(list rows, int width=0, bool sparse=false, int max_basis=0) dict {
   "Return right-nullspace diagnostics using packed GF(2) row elimination."
   def t0 = ticks()
   def packed_info = _gf2_pack_rows(rows, width, sparse)
   _gf2_packed_rows_nullspace_report(packed_info.get(0), int(packed_info.get(1, width)), max_basis, "packed-gf2-elimination", t0)
}

fn packed_gf2_nullspace(list rows, int width=0, bool sparse=false) list {
   "Return a dense basis from packed_gf2_nullspace_report."
   packed_gf2_nullspace_report(rows, width, sparse).get("basis", [])
}

fn _gf2_zero_vec(int n) list {
   mut out = list(n)
   mut i = 0
   while i < n {
      out[i] = 0
      i += 1
   }
   out
}

fn _gf2_xor_vec(list<int> a, list<int> b) list {
   def n = a.len
   mut out = list(n)
   mut i = 0
   while i < n {
      out[i] = (int(a[i]) ^^ int(b[i])) & 1
      i += 1
   }
   out
}

fn _gf2_vec_is_zero(list<int> v) bool {
   def n = v.len
   mut i = 0
   while i < n {
      if (int(v[i]) & 1) != 0 { return false }
      i += 1
   }
   true
}

fn _gf2_vec_weight(list<int> v) int {
   def n = v.len
   mut w, i = 0, 0
   while i < n {
      if (int(v[i]) & 1) != 0 { w += 1 }
      i += 1
   }
   w
}

fn _gf2_vec_pivot(list<int> v) int {
   def n = v.len
   mut i = 0
   while i < n {
      if (int(v[i]) & 1) != 0 { return i }
      i += 1
   }
   -1
}

fn _gf2_dense_width(list rows, int width=0) int {
   mut w = width
   if w <= 0 && rows.len > 0 { w = rows.get(0).len }
   w
}

fn _gf2_append_all(list a, list b) list {
   mut out = a
   mut i = 0
   while i < b.len {
      out = out.append(b.get(i))
      i += 1
   }
   out
}

fn _gf2_cycle_to_dependency(list cycle, int width) list {
   mut out = _gf2_zero_vec(width)
   mut i = 0
   while i < cycle.len {
      def c = int(cycle.get(i))
      if c >= 0 && c < width { out[c] = int(out.get(c, 0)) ^^ 1 }
      i += 1
   }
   out
}

fn _gf2_precondition_row_counts2(list active, list col_rows, int nrows) list {
   mut counts = _gf2_zero_vec(nrows)
   mut j = 0
   while j < col_rows.len {
      if j < active.len && bool(active[j]) {
         def cr = col_rows[j]
         mut i = 0
         while i < cr.len {
            def r = int(cr[i])
            if r >= 0 && r < nrows { counts[r] = int(counts[r]) + 1 }
            i += 1
         }
      }
      j += 1
   }
   counts
}

fn _gf2_precondition_active_cols2(list active) int {
   mut n, j = 0, 0
   while j < active.len {
      if bool(active[j]) { n += 1 }
      j += 1
   }
   n
}

fn _gf2_precondition_find_other_col2(list active, list col_rows, int row, int skip) int {
   mut j = 0
   while j < col_rows.len {
      if j != skip && j < active.len && bool(active[j]) && _gf2_sparse_contains(col_rows[j], row) { return j }
      j += 1
   }
   -1
}

fn _gf2_precondition_decrement_intersection_counts(list counts, list a, list b) list {
   mut i, j = 0, 0
   while i < a.len && j < b.len {
      def av = int(a[i])
      def bv = int(b[j])
      if av == bv {
         if av >= 0 && av < counts.len { counts[av] = int(counts[av]) - 2 }
         i += 1
         j += 1
      } else if av < bv {
         i += 1
      } else {
         j += 1
      }
   }
   counts
}

fn _gf2_precondition_active_rows2(list active, list col_rows, int nrows) list {
   def counts = _gf2_precondition_row_counts2(active, col_rows, nrows)
   mut active_count = 0
   mut i = 0
   while i < nrows {
      if int(counts[i]) > 0 { active_count += 1 }
      i += 1
   }
   mut out = list(active_count)
   i = 0
   mut oi = 0
   while i < nrows {
      if int(counts[i]) > 0 {
         out[oi] = i
         oi += 1
      }
      i += 1
   }
   out
}

fn _gf2_precondition_initial_columns(list rows, int width) dict {
   mut active = list(width)
   mut col_rows_all = list(width)
   mut col_cycles_all = list(width)
   mut j = 0
   while j < width {
      active[j] = true
      col_rows_all[j] = []
      col_cycles_all[j] = [j]
      j += 1
   }
   def sparse_rows = _gf2_sparse_from_dense_rows(rows)
   mut input_nonzeros = 0
   mut r = 0
   while r < sparse_rows.len {
      def row = sparse_rows.get(r)
      input_nonzeros += row.len
      mut i = 0
      while i < row.len {
         def c = int(row.get(i))
         if c >= 0 && c < width { col_rows_all[c] = col_rows_all.get(c).append(r) }
         i += 1
      }
      r += 1
   }
   {"active": active, "col_rows": col_rows_all, "col_cycles": col_cycles_all, "sparse_rows": sparse_rows, "input_nonzeros": input_nonzeros}
}

fn _gf2_precondition_nochange_state(list rows, list col_rows_all, list col_cycles_all, int width) dict {
   mut row_map = list(rows.len)
   mut col_map = list(width)
   mut i = 0
   while i < rows.len {
      row_map[i] = i
      i += 1
   }
   i = 0
   while i < width {
      col_map[i] = i
      i += 1
   }
   {"row_map": row_map, "col_map": col_map, "col_cycles": col_cycles_all, "col_rows": col_rows_all, "reduced_rows": rows}
}

fn _gf2_precondition_quick_noop(list rows, int width, bool prune_singletons, bool collapse_cliques) bool {
   if width <= 0 { return true }
   mut counts = _gf2_zero_vec(width)
   mut can_clique = false
   mut r = 0
   while r < rows.len {
      def row = rows.get(r)
      mut row_weight = 0
      mut c = 0
      while c < row.len && c < width {
         if (int(row.get(c, 0)) & 1) != 0 {
            counts[c] = int(counts.get(c, 0)) + 1
            row_weight += 1
         }
         c += 1
      }
      if collapse_cliques && row_weight == 2 { can_clique = true }
      r += 1
   }
   if can_clique { return false }
   mut j = 0
   while j < width {
      def n = int(counts.get(j, 0))
      if n == 0 || (prune_singletons && n == 1) { return false }
      j += 1
   }
   true
}

fn _gf2_precondition_prune_pass(list active, list col_rows_all, list col_cycles_all, int row_count, int width, bool prune_singletons) dict {
   def row_counts = _gf2_precondition_row_counts2(active, col_rows_all, row_count)
   mut empty_columns_removed, singleton_columns_removed, immediate_dependencies, changed, j = 0, 0, [], false, 0
   while j < active.len {
      if bool(active[j]) {
         def cr = col_rows_all[j]
         mut drop = cr.len == 0
         if prune_singletons && !drop {
            mut i = 0
            while i < cr.len && !drop {
               def r = int(cr[i])
               if r >= 0 && r < row_counts.len && int(row_counts[r]) <= 1 { drop = true }
               i += 1
            }
         }
         if drop {
            if cr.len == 0 { immediate_dependencies = immediate_dependencies.append(_gf2_cycle_to_dependency(col_cycles_all[j], width)) }
            active[j] = false
            if cr.len == 0 { empty_columns_removed += 1 } else { singleton_columns_removed += 1 }
            changed = true
         }
      }
      j += 1
   }
   {"changed": changed, "empty": empty_columns_removed, "singleton": singleton_columns_removed, "dependencies": immediate_dependencies}
}

fn _gf2_precondition_clique_pass(list active, list col_rows_all, list col_cycles_all, int row_count) dict {
   mut changed, merged, clique_merges = false, true, 0
   def row_counts = _gf2_precondition_row_counts2(active, col_rows_all, row_count)
   while merged {
      merged = false
      mut j = 0
      while j < active.len && !merged {
         if bool(active[j]) {
            def c1_rows = col_rows_all[j]
            mut base, i = -1, 0
            while i < c1_rows.len && base < 0 {
               def r = int(c1_rows[i])
               if r >= 0 && r < row_counts.len && int(row_counts[r]) == 2 { base = _gf2_precondition_find_other_col2(active, col_rows_all, r, j) }
               i += 1
            }
            def c0_rows = base >= 0 ? col_rows_all[base] : []
            if base >= 0 && base < active.len && bool(active[base]) && c0_rows.len + c1_rows.len < 1000 {
               _gf2_precondition_decrement_intersection_counts(row_counts, c0_rows, c1_rows)
               col_rows_all[base] = _gf2_sparse_xor(c0_rows, c1_rows)
               col_cycles_all[base] = _gf2_append_all(col_cycles_all[base], col_cycles_all[j])
               active[j] = false
               clique_merges += 1
               changed, merged = true, true
            }
         }
         j += 1
      }
   }
   {"changed": changed, "merges": clique_merges}
}

fn _gf2_precondition_trim_heavy(list active, list col_rows_all, int row_count, int num_excess) int {
   mut keep_limit = _gf2_precondition_active_rows2(active, col_rows_all, row_count).len + max(0, num_excess)
   mut trimmed = 0
   while _gf2_precondition_active_cols2(active) > keep_limit {
      mut best_col, best_weight, j = -1, -1, 0
      while j < active.len {
         if bool(active[j]) {
            def cw = col_rows_all[j].len
            if cw > best_weight { best_weight, best_col = cw, j }
         }
         j += 1
      }
      if best_col < 0 { keep_limit = _gf2_precondition_active_cols2(active) } else {
         active[best_col] = false
         trimmed += 1
      }
   }
   trimmed
}

fn _gf2_precondition_reduced_state(list active, list col_rows_all, list col_cycles_all, int row_count) dict {
   def row_map = _gf2_precondition_active_rows2(active, col_rows_all, row_count)
   def active_cols = _gf2_precondition_active_cols2(active)
   mut col_map = list(active_cols)
   mut col_cycles = list(active_cols)
   mut col_rows = list(active_cols)
   mut j = 0
   mut out_j = 0
   while j < active.len {
      if active.get(j, false) {
         col_map[out_j] = j
         col_cycles[out_j] = col_cycles_all.get(j)
         col_rows[out_j] = col_rows_all.get(j)
         out_j += 1
      }
      j += 1
   }
   mut reduced, i = [], 0
   while i < row_map.len {
      mut out_row = []
      j = 0
      while j < col_rows.len {
         out_row = out_row.append(_gf2_sparse_contains(col_rows.get(j), int(row_map.get(i))) ? 1 : 0)
         j += 1
      }
      reduced = reduced.append(out_row)
      i += 1
   }
   {"row_map": row_map, "col_map": col_map, "col_cycles": col_cycles, "col_rows": col_rows, "reduced_rows": reduced}
}

fn gf2_matrix_precondition_report(list rows, int width=0, int num_excess=8, bool prune_singletons=true, bool trim_heavy=false, bool collapse_cliques=false) dict {
   "Return a sparse GF(2) matrix preconditioning report for dependency solving."
   def t0 = ticks()
   def w = _gf2_dense_width(rows, width)
   def initial = _gf2_precondition_initial_columns(rows, w)
   def active, col_rows_all, col_cycles_all = initial.get("active"), initial.get("col_rows"), initial.get("col_cycles")
   def input_nonzeros = int(initial.get("input_nonzeros", 0))
   mut passes, empty_columns_removed, singleton_columns_removed, clique_merges = 0, 0, 0, 0
   mut immediate_dependencies, changed = [], true
   while changed {
      passes += 1
      def pruned = _gf2_precondition_prune_pass(active, col_rows_all, col_cycles_all, rows.len, w, prune_singletons)
      empty_columns_removed += int(pruned.get("empty", 0))
      singleton_columns_removed += int(pruned.get("singleton", 0))
      immediate_dependencies = _gf2_append_all(immediate_dependencies, pruned.get("dependencies", []))
      changed = pruned.get("changed", false)
      if collapse_cliques {
         def cliques = _gf2_precondition_clique_pass(active, col_rows_all, col_cycles_all, rows.len)
         clique_merges += int(cliques.get("merges", 0))
         changed = changed || cliques.get("changed", false)
      }
   }
   def trimmed_heavy_columns = trim_heavy ? _gf2_precondition_trim_heavy(active, col_rows_all, rows.len, num_excess) : 0
   def nochange = empty_columns_removed == 0 && singleton_columns_removed == 0 && clique_merges == 0 && trimmed_heavy_columns == 0
   def state = nochange ? _gf2_precondition_nochange_state(rows, col_rows_all, col_cycles_all, w) : _gf2_precondition_reduced_state(active, col_rows_all, col_cycles_all, rows.len)
   def row_map, col_map, col_cycles = state.get("row_map"), state.get("col_map"), state.get("col_cycles")
   def col_rows, reduced = state.get("col_rows"), state.get("reduced_rows")
   _finish_report_with(_report("gf2-matrix-precondition", 24), t0, [
         ["input_rows", rows.len], ["input_cols", w],
         ["output_rows", reduced.len], ["output_cols", col_map.len],
         ["input_nonzeros", input_nonzeros],
         ["output_nonzeros", _gf2_sparse_entry_count(col_rows)],
         ["passes", passes], ["prune_singletons", prune_singletons],
         ["trim_heavy", trim_heavy], ["collapse_cliques", collapse_cliques],
         ["num_excess", num_excess], ["empty_rows_removed", rows.len - row_map.len],
         ["empty_columns_removed", empty_columns_removed],
         ["singleton_columns_removed", singleton_columns_removed],
         ["clique_merges", clique_merges], ["immediate_dependencies", immediate_dependencies],
         ["immediate_dependency_count", immediate_dependencies.len],
         ["trimmed_heavy_columns", trimmed_heavy_columns],
         ["row_map", row_map], ["col_map", col_map],
         ["col_cycles", col_cycles], ["reduced_rows", reduced],
      ])
}

fn _gf2_seed_vec(int width, int seed) list {
   mut out = _gf2_zero_vec(width)
   if width <= 0 { return out }
   if seed < width {
      out[seed] = 1
      return out
   }
   mut state = (seed + 1) * 1103515245 + 12345
   mut i = 0
   while i < width {
      state = (state * 1103515245 + 12345) & 1073741823
      if ((state >> 7) & 1) == 1 { out[i] = 1 }
      i += 1
   }
   if _gf2_vec_is_zero(out) { out[seed % width] = 1 }
   out
}

fn _gf2_columns_to_rows(list cols, int row_count) list {
   mut rows = list(row_count)
   mut r = 0
   while r < row_count {
      def col_count = cols.len
      mut row = list(col_count)
      mut c = 0
      while c < col_count {
         row[c] = int(cols.get(c).get(r, 0)) & 1
         c += 1
      }
      rows[r] = row
      r += 1
   }
   rows
}

fn _gf2_independent_basis_report(list candidates, int width=0) dict {
   def w = candidates.len > 0 ? _gf2_dense_width([candidates[0]], width) : width
   mut out = list(candidates.len)
   mut reduced_rows = list(candidates.len)
   mut pivot_index = list(max(0, w))
   mut pi = 0
   while pi < pivot_index.len {
      pivot_index[pi] = -1
      pi += 1
   }
   mut rank = 0
   mut reduction_xors = 0
   mut dependent_skips = 0
   mut i = 0
   while i < candidates.len {
      def v = candidates[i]
      if !_gf2_vec_is_zero(v) {
         mut red = _gf2_copy_vec(v)
         mut p = _gf2_vec_pivot(red)
         while p >= 0 && p < pivot_index.len && int(pivot_index[p]) >= 0 {
            def idx = int(pivot_index[p])
            def list reduced_row = reduced_rows.get(idx)
            _gf2_xor_vec_into(red, reduced_row, w)
            reduction_xors += 1
            p = _gf2_vec_pivot(red)
         }
         if p >= 0 {
            if p < pivot_index.len { pivot_index[p] = rank }
            reduced_rows[rank] = red
            out[rank] = v
            rank += 1
         } else {
            dependent_skips += 1
         }
      }
      i += 1
   }
   __list_set_len(out, rank)
   __list_set_len(reduced_rows, rank)
   _dict_with(10, [
         ["method", "incremental-gf2-independence"],
         ["input_count", candidates.len], ["cols", w], ["rank", rank],
         ["basis", out], ["reduced_rows", reduced_rows],
         ["reduction_xors", reduction_xors], ["dependent_skips", dependent_skips],
      ])
}

fn _gf2_verified_basis_report(list rows, list candidates, int width=0) dict {
   def w = _gf2_dense_width(rows, width)
   def packed_rows = _gf2_pack_rows(rows, w, false).get(0)
   mut verified = list(candidates.len)
   mut verified_count = 0
   mut seen = dict()
   mut duplicate_skips = 0
   mut zero_skips = 0
   mut invalid_skips = 0
   mut matvec_checks = 0
   mut seen_key_pack_ops = 0
   mut seen_key_words = 0
   mut packed_word_and_ops = 0
   mut packed_popcount_ops = 0
   mut i = 0
   while i < candidates.len {
      def v = candidates[i]
      def key_info = _gf2_dense_vec_packed_words(v, w)
      def nonzero_vec = bool(key_info[0])
      def words = key_info[2]
      seen_key_pack_ops += 1
      seen_key_words += int(key_info[1])
      if !nonzero_vec {
         zero_skips += 1
      } else {
         def seen_add = _gf2_seen_packed_words_add(seen, words)
         if bool(seen_add[0]) {
            duplicate_skips += 1
         } else {
            seen = seen_add[1]
            matvec_checks += 1
            def mv = _gf2_packed_matvec_is_zero(packed_rows, words)
            packed_word_and_ops += int(mv[1])
            packed_popcount_ops += int(mv[2])
            if bool(mv[0]) {
               verified[verified_count] = v
               verified_count += 1
            } else {
               invalid_skips += 1
            }
         }
      }
      i += 1
   }
   __list_set_len(verified, verified_count)
   mut rep = _gf2_independent_basis_report(verified, w)
   _set_fields(rep, [
         ["method", "verified-incremental-gf2-basis"],
         ["candidate_count", candidates.len], ["verified_candidates", verified.len],
         ["matvec_checks", matvec_checks], ["duplicate_skips", duplicate_skips],
         ["zero_skips", zero_skips], ["invalid_skips", invalid_skips],
         ["matrix_kernel", "packed-row-popcount"],
         ["dense_entry_dot_ops", 0],
         ["packed_word_and_ops", packed_word_and_ops],
         ["packed_popcount_ops", packed_popcount_ops],
         ["seen_key_pack_ops", seen_key_pack_ops],
         ["seen_key_words", seen_key_words],
      ])
}

fn _gf2_verified_sparse_basis_report(list sparse_rows, list candidates, int width=0) dict {
   mut w = width
   if w <= 0 && candidates.len > 0 { w = _gf2_dense_width([candidates.get(0)], width) }
   if w <= 0 {
      mut max_col = -1
      mut i = 0
      while i < sparse_rows.len {
         def row = sparse_rows.get(i)
         mut j = 0
         while j < row.len {
            def col = int(row.get(j, -1))
            if col > max_col { max_col = col }
            j += 1
         }
         i += 1
      }
      w = max_col + 1
   }
   mut verified = list(candidates.len)
   mut verified_count = 0
   mut seen = dict()
   mut duplicate_skips = 0
   mut zero_skips = 0
   mut invalid_skips = 0
   mut matvec_checks = 0
   mut sparse_entry_dot_ops = 0
   def sparse_entries = _gf2_sparse_entry_count(sparse_rows)
   mut seen_key_pack_ops = 0
   mut seen_key_words = 0
   mut dense_seen_key_string_ops = 0
   mut i = 0
   while i < candidates.len {
      def v = candidates.get(i)
      def key_info = _gf2_dense_vec_packed_words(v, w)
      def nonzero_vec = bool(key_info[0])
      def words = key_info[2]
      seen_key_pack_ops += 1
      seen_key_words += int(key_info[1])
      if !nonzero_vec {
         zero_skips += 1
      } else {
         def seen_add = _gf2_seen_packed_words_add(seen, words)
         if bool(seen_add[0]) {
            duplicate_skips += 1
         } else {
            seen = seen_add[1]
            matvec_checks += 1
            sparse_entry_dot_ops += sparse_entries
            if _gf2_sparse_matvec_is_zero(sparse_rows, v) {
               verified[verified_count] = v
               verified_count += 1
            } else {
               invalid_skips += 1
            }
         }
      }
      i += 1
   }
   __list_set_len(verified, verified_count)
   mut rep = _gf2_independent_basis_report(verified, w)
   _set_fields(rep, [
         ["method", "verified-sparse-gf2-basis"],
         ["candidate_count", candidates.len], ["verified_candidates", verified.len],
         ["matvec_checks", matvec_checks], ["duplicate_skips", duplicate_skips],
         ["zero_skips", zero_skips], ["invalid_skips", invalid_skips],
         ["matrix_kernel", "sparse-row-index"],
         ["sparse_nonzeros", _gf2_sparse_entry_count(sparse_rows)],
         ["sparse_entry_dot_ops", sparse_entry_dot_ops],
         ["seen_key_pack_ops", seen_key_pack_ops], ["seen_key_words", seen_key_words],
         ["dense_seen_key_string_ops", dense_seen_key_string_ops],
      ])
}

fn _gf2_block_krylov_core_fields(str vector_count_key, bool use_sparse_kernel, list rows, list sparse_work_rows, int w, int bs, int iters, bool bounded_default_iters, int vector_count, bool image_packed_transpose, int image_packed_row_words, int sparse_entries, int packed_row_word_ops) list {
   [
      ["rows", rows.len], ["cols", w], ["block_size", bs], ["max_iters", iters],
      ["bounded_default_iters", bounded_default_iters], [vector_count_key, vector_count],
      ["image_rows", use_sparse_kernel ? sparse_work_rows.len : rows.len], ["image_cols", vector_count],
      ["image_packed_transpose", image_packed_transpose], ["image_packed_row_words", image_packed_row_words],
      ["matrix_kernel", use_sparse_kernel ? "sparse-row-index" : "packed-row-popcount"],
      ["sparse_nonzeros", sparse_entries], ["packed_row_word_ops", packed_row_word_ops],
   ]
}

fn _gf2_block_krylov_post_lanczos_fields(bool enabled, int rows_saved, int active_rows, int saved_entries, int active_entries, int saved_row_words, int saved_matrix_words, int matvecs) list {
   [
      ["post_lanczos_pack_enabled", enabled],
      ["post_lanczos_policy", enabled ? "dense-prefix-split" : "disabled"],
      ["post_lanczos_source_model", "Block Lanczos dense-prefix post-processing split"],
      ["post_lanczos_rows_saved", rows_saved], ["post_lanczos_active_rows", active_rows],
      ["post_lanczos_saved_entries", saved_entries], ["post_lanczos_active_entries", active_entries],
      ["post_lanczos_saved_row_words", saved_row_words], ["post_lanczos_saved_matrix_words", saved_matrix_words],
      ["post_lanczos_entry_ops_avoided_est", saved_entries * matvecs],
   ]
}

fn _gf2_block_krylov_op_fields(
   int w, int matvecs, int normal_matvecs,
   int packed_word_and_ops, int packed_popcount_ops,
   int packed_transpose_row_xor_ops, int packed_transpose_word_xor_ops,
   int sparse_entry_dot_ops, int sparse_transpose_row_xor_ops, int sparse_transpose_entry_xor_ops,
   int packed_vector_pack_ops, int packed_vector_decode_ops, int packed_vector_repack_avoided,
   int sparse_vector_pack_ops_avoided, int sparse_seen_key_pack_ops, int sparse_seen_key_words,
) list {
   [
      ["word_bits", _gf2_word_bits()], ["word_count", _gf2_word_count(w)],
      ["matvecs", matvecs], ["normal_matvecs", normal_matvecs],
      ["packed_word_and_ops", packed_word_and_ops], ["packed_popcount_ops", packed_popcount_ops],
      ["packed_transpose_row_xor_ops", packed_transpose_row_xor_ops], ["packed_transpose_word_xor_ops", packed_transpose_word_xor_ops],
      ["sparse_entry_dot_ops", sparse_entry_dot_ops], ["sparse_transpose_row_xor_ops", sparse_transpose_row_xor_ops],
      ["sparse_transpose_entry_xor_ops", sparse_transpose_entry_xor_ops],
      ["packed_vector_pack_ops", packed_vector_pack_ops], ["packed_vector_decode_ops", packed_vector_decode_ops],
      ["packed_vector_repack_avoided", packed_vector_repack_avoided],
      ["sparse_vector_pack_ops_avoided", sparse_vector_pack_ops_avoided],
      ["sparse_seen_key_pack_ops", sparse_seen_key_pack_ops], ["sparse_seen_key_words", sparse_seen_key_words],
   ]
}

fn _gf2_block_krylov_solution_fields(list deps, int image_basis_limit, list candidates, int iterative_verified, dict verified_report, int exact_nullity, int exact_rank, list basis, list weights, bool complete, int exact_added, dict exact_closure_report, dict image_la, dict exact) list {
   [
      ["dependency_count", deps.len], ["dependency_limit", image_basis_limit],
      ["candidate_count", candidates.len], ["iterative_verified_count", iterative_verified],
      ["verified_basis_report", verified_report],
      ["verification_matrix_kernel", verified_report.get("matrix_kernel", "dense-row-scan")],
      ["exact_nullity", exact_nullity], ["exact_rank", exact_rank],
      ["basis", basis], ["basis_weights", weights], ["verified_count", basis.len],
      ["complete", complete && basis.len == exact_nullity],
      ["exact_closure", complete && exact_added > 0], ["exact_added", exact_added],
      ["exact_closure_report", exact_closure_report],
      ["krylov_linear_algebra", image_la], ["exact_linear_algebra", exact],
   ]
}

fn _gf2_block_krylov_candidates(bool use_sparse_kernel, list deps, int vector_count, int w, list vectors, list vectors_packed) dict {
   def dep_count = deps.len
   mut candidates = list(dep_count)
   __list_set_len(candidates, dep_count)
   def word_count = _gf2_word_count(w)
   mut packed_vector_decode_ops = 0
   mut candidate_packed_xor_ops = 0
   mut candidate_dense_xor_ops = 0
   mut candidate_dense_xor_avoided = 0
   def packed_candidate_path = vectors_packed.len >= vector_count
   if packed_candidate_path || !use_sparse_kernel {
      mut i = 0
      while i < dep_count {
         def dep = deps.get(i)
         mut cand_words = _gf2_zero_words(word_count)
         mut j = 0
         while j < dep.len && j < vector_count {
            if (int(dep.get(j, 0)) & 1) != 0 {
               cand_words = _gf2_packed_xor_inplace(cand_words, vectors_packed.get(j), 0)
               candidate_packed_xor_ops += word_count
               if use_sparse_kernel { candidate_dense_xor_avoided += w }
            }
            j += 1
         }
         candidates[i] = _gf2_packed_to_dense(cand_words, w)
         packed_vector_decode_ops += 1
         i += 1
      }
   } else {
      mut i = 0
      while i < dep_count {
         def dep = deps.get(i)
         mut cand = []
         mut j = 0
         while j < dep.len && j < vector_count {
            if (int(dep.get(j, 0)) & 1) != 0 {
               cand = _gf2_xor_vec(cand, vectors.get(j))
               candidate_dense_xor_ops += w
            }
            j += 1
         }
         candidates[i] = cand
         i += 1
      }
   }
   _dict_with(10, [
         ["candidates", candidates],
         ["packed_vector_decode_ops", packed_vector_decode_ops],
         ["candidate_packed_xor_enabled", packed_candidate_path],
         ["candidate_packed_xor_ops", candidate_packed_xor_ops],
         ["candidate_dense_xor_ops", candidate_dense_xor_ops],
         ["candidate_dense_xor_avoided", candidate_dense_xor_avoided],
      ])
}

fn _gf2_block_krylov_image_report(bool use_sparse_kernel, list images, list sparse_work_rows, list rows, int vector_count, int image_basis_limit) dict {
   mut image_packed_transpose = false
   mut image_packed_row_words = 0
   mut image_la = dict()
   if use_sparse_kernel {
      def image_packed_rows = _gf2_columns_to_packed_rows(images, sparse_work_rows.len)
      image_packed_transpose = true
      image_packed_row_words = image_packed_rows.len > 0 ? image_packed_rows.get(0).len : 0
      image_la = _gf2_packed_rows_nullspace_report(image_packed_rows, vector_count, image_basis_limit, "packed-gf2-elimination-packed-image")
   } else {
      def image_rows = _gf2_columns_to_rows(images, rows.len)
      image_la = packed_gf2_nullspace_report(image_rows, vector_count, false, image_basis_limit)
   }
   _dict_with(3, [["linear_algebra", image_la], ["packed_transpose", image_packed_transpose], ["packed_row_words", image_packed_row_words]])
}

fn _gf2_block_krylov_op_fields_from(int w, dict walk, int extra_decode_ops) list {
   _gf2_block_krylov_op_fields(w,
      int(walk.get("matvecs", 0)),
      int(walk.get("normal_matvecs", 0)),
      int(walk.get("packed_word_and_ops", 0)),
      int(walk.get("packed_popcount_ops", 0)),
      int(walk.get("packed_transpose_row_xor_ops", 0)),
      int(walk.get("packed_transpose_word_xor_ops", 0)),
      int(walk.get("sparse_entry_dot_ops", 0)),
      int(walk.get("sparse_transpose_row_xor_ops", 0)),
      int(walk.get("sparse_transpose_entry_xor_ops", 0)),
      int(walk.get("packed_vector_pack_ops", 0)),
      int(walk.get("packed_vector_decode_ops", 0)) + extra_decode_ops,
      int(walk.get("packed_vector_repack_avoided", 0)),
      int(walk.get("sparse_vector_pack_ops_avoided", 0)),
      int(walk.get("sparse_seen_key_pack_ops", 0)),
      int(walk.get("sparse_seen_key_words", 0)),
   )
}

fn _gf2_block_krylov_solution_report(bool use_sparse_kernel, list rows, list sparse_rows, int w, list deps, int vector_count, list vectors, list vectors_packed, int image_basis_limit, bool complete, dict image_la) dict {
   def candidate_report = _gf2_block_krylov_candidates(use_sparse_kernel, deps, vector_count, w, vectors, vectors_packed)
   def candidates = candidate_report.get("candidates", [])
   def verified_report = use_sparse_kernel ? _gf2_verified_sparse_basis_report(sparse_rows, candidates, w) : _gf2_verified_basis_report(rows, candidates, w)
   mut basis = verified_report.get("basis", [])
   def iterative_verified = basis.len
   mut exact = dict()
   mut exact_basis, exact_nullity, exact_rank = [], -1, -1
   if complete {
      exact = packed_gf2_nullspace_report(rows, w, false)
      exact_basis = exact.get("basis", [])
      exact_nullity = exact.get("nullity", 0)
      exact_rank = exact.get("rank", 0)
   }
   mut exact_added = 0
   mut exact_closure_report = dict()
   if complete && basis.len < exact_basis.len {
      def combined = _list_concat(basis, exact_basis)
      exact_closure_report = _gf2_independent_basis_report(combined, w)
      basis = exact_closure_report.get("basis", basis)
      exact_added = basis.len - iterative_verified
   }
   mut weights = list(basis.len)
   __list_set_len(weights, basis.len)
   mut i = 0
   while i < basis.len {
      weights[i] = _gf2_vec_weight(basis.get(i))
      i += 1
   }
   def fields = _fields_extend(_gf2_block_krylov_solution_fields(deps, image_basis_limit, candidates, iterative_verified, verified_report, exact_nullity, exact_rank, basis, weights, complete, exact_added, exact_closure_report, image_la, exact), [
         ["candidate_packed_xor_enabled", candidate_report.get("candidate_packed_xor_enabled", false)],
         ["candidate_packed_xor_ops", candidate_report.get("candidate_packed_xor_ops", 0)],
         ["candidate_dense_xor_ops", candidate_report.get("candidate_dense_xor_ops", 0)],
         ["candidate_dense_xor_avoided", candidate_report.get("candidate_dense_xor_avoided", 0)],
      ])
   _dict_with(2, [
         ["packed_vector_decode_ops", int(candidate_report.get("packed_vector_decode_ops", 0))],
         ["fields", fields],
      ])
}

fn _gf2_block_krylov_walk_report(bool use_sparse_kernel, bool shifted_seed, list packed_rows, list sparse_work_rows, int sparse_work_entries, int w, int bs, int iters) dict {
   mut vectors = []
   def max_vectors = max(0, bs * max(0, iters))
   mut vectors_packed = list(max_vectors)
   mut images = list(max_vectors)
   __list_set_len(vectors_packed, max_vectors)
   __list_set_len(images, max_vectors)
   mut vector_count = 0
   mut seen = dict()
   mut matvecs, normal_matvecs = 0, 0
   mut packed_word_and_ops, packed_popcount_ops = 0, 0
   mut packed_transpose_row_xor_ops, packed_transpose_word_xor_ops = 0, 0
   mut sparse_entry_dot_ops, sparse_transpose_row_xor_ops, sparse_transpose_entry_xor_ops = 0, 0, 0
   mut packed_vector_pack_ops, packed_vector_decode_ops, packed_vector_repack_avoided = 0, 0, 0
   mut sparse_vector_pack_ops_avoided, sparse_seen_key_pack_ops, sparse_seen_key_words = 0, 0, 0
   mut sparse_packed_matvecs, sparse_packed_transpose_matvecs = 0, 0
   mut sparse_packed_bit_reads = 0
   mut sparse_word_dot_ops, sparse_word_transpose_xor_ops = 0, 0
   mut sparse_dense_vector_materializations_avoided = 0
   mut sparse_seen_key_repack_avoided = 0
   mut sparse_dual_transpose_matvecs = 0
   mut sparse_transpose_counts = use_sparse_kernel ? [0, 0] : []
   mut seed = 0
   def sparse_image_len = use_sparse_kernel ? sparse_work_rows.len : 0
   while seed < bs {
      def seed_col = shifted_seed ? seed + bs : seed
      mut v = _gf2_seed_vec(w, seed_col)
      mut v_words = _gf2_pack_dense_row(v, w)
      mut image_buf = use_sparse_kernel ? _gf2_zero_vec(sparse_image_len) : []
      packed_vector_pack_ops += use_sparse_kernel ? 0 : 1
      mut iter = 0
      while iter < iters {
         mut image = []
         if use_sparse_kernel {
            sparse_vector_pack_ops_avoided += 1
            image = _gf2_sparse_matvec_into(sparse_work_rows, v, image_buf)
            sparse_entry_dot_ops += sparse_work_entries
         } else {
            def image_pack = _gf2_packed_matvec_words(packed_rows, v_words)
            image = image_pack.get(0)
            packed_word_and_ops += int(image_pack.get(1, 0))
            packed_popcount_ops += int(image_pack.get(2, 0))
         }
         matvecs += 1
         mut nonzero_vec = false
         mut v_key_words = []
         if use_sparse_kernel {
            def key_info = _gf2_dense_vec_packed_words(v, w)
            nonzero_vec = bool(key_info[0])
            v_key_words = key_info[2]
            sparse_seen_key_pack_ops += 1
            sparse_seen_key_words += int(key_info[1])
         } else {
            nonzero_vec = !_gf2_packed_is_zero(v_words)
            v_key_words = v_words
         }
         def seen_add = nonzero_vec ? _gf2_seen_packed_words_add(seen, v_key_words) : [true, seen]
         if nonzero_vec && !bool(seen_add[0]) {
            seen = seen_add[1]
            if use_sparse_kernel {
               vectors_packed[vector_count] = v_key_words
               sparse_dense_vector_materializations_avoided += w
            } else {
               vectors_packed[vector_count] = v_words
            }
            images[vector_count] = use_sparse_kernel ? _gf2_copy_vec(image) : image
            vector_count += 1
         }
         if use_sparse_kernel {
            def nonzero_step = _gf2_sparse_transpose_matvec_into(sparse_work_rows, image, v, sparse_transpose_counts)
            sparse_packed_transpose_matvecs += 1
            if !nonzero_step { iter = iters }
         } else {
            def normal_pack = _gf2_packed_transpose_matvec_words_only(packed_rows, image, w)
            v_words = normal_pack[2]
            packed_vector_repack_avoided += 1
            packed_transpose_row_xor_ops += int(normal_pack[0])
            packed_transpose_word_xor_ops += int(normal_pack[1])
         }
         normal_matvecs += 1
         if !use_sparse_kernel && _gf2_packed_is_zero(v_words) { iter = iters }
         iter += 1
      }
      seed += 1
   }
   if use_sparse_kernel {
      sparse_transpose_row_xor_ops = sparse_transpose_counts[0]
      sparse_transpose_entry_xor_ops = sparse_transpose_counts[1]
   }
   __list_set_len(vectors_packed, vector_count)
   __list_set_len(images, vector_count)
   _dict_with(24, [
         ["vectors", vectors], ["vectors_packed", vectors_packed], ["images", images],
         ["matvecs", matvecs], ["normal_matvecs", normal_matvecs],
         ["packed_word_and_ops", packed_word_and_ops], ["packed_popcount_ops", packed_popcount_ops],
         ["packed_transpose_row_xor_ops", packed_transpose_row_xor_ops], ["packed_transpose_word_xor_ops", packed_transpose_word_xor_ops],
         ["sparse_entry_dot_ops", sparse_entry_dot_ops], ["sparse_transpose_row_xor_ops", sparse_transpose_row_xor_ops],
         ["sparse_transpose_entry_xor_ops", sparse_transpose_entry_xor_ops],
         ["packed_vector_pack_ops", packed_vector_pack_ops], ["packed_vector_decode_ops", packed_vector_decode_ops],
         ["packed_vector_repack_avoided", packed_vector_repack_avoided],
         ["sparse_vector_pack_ops_avoided", sparse_vector_pack_ops_avoided],
         ["sparse_seen_key_pack_ops", sparse_seen_key_pack_ops], ["sparse_seen_key_words", sparse_seen_key_words],
         ["sparse_packed_matvec_enabled", false], ["sparse_packed_transpose_enabled", false],
         ["sparse_packed_matvecs", sparse_packed_matvecs], ["sparse_packed_transpose_matvecs", sparse_packed_transpose_matvecs],
         ["sparse_packed_bit_reads", sparse_packed_bit_reads],
         ["sparse_word_dot_ops", sparse_word_dot_ops], ["sparse_word_transpose_xor_ops", sparse_word_transpose_xor_ops],
         ["sparse_dense_vector_materializations_avoided", sparse_dense_vector_materializations_avoided],
         ["sparse_seen_key_repack_avoided", sparse_seen_key_repack_avoided],
         ["sparse_dual_transpose_enabled", use_sparse_kernel],
         ["sparse_dual_transpose_matvecs", sparse_dual_transpose_matvecs],
      ])
}

fn _gf2_block_krylov_iteration_plan(int width, int block_size, int max_iters, bool complete) dict {
   mut bs = block_size
   if bs <= 0 { bs = 8 }
   if bs > max(1, width) { bs = max(1, width) }
   mut iters = max_iters
   mut bounded_default_iters = false
   if iters <= 0 {
      if complete {
         iters = width + 2
      } else {
         iters = min(width + 2, max(16, bs * 2))
         bounded_default_iters = true
      }
   }
   if width <= 0 { iters = 0 }
   _dict_with(4, [["block_size", bs], ["iters", iters], ["bounded_default_iters", bounded_default_iters]])
}

fn _gf2_block_krylov_setup(list rows, int width, int block_size, int max_iters, bool complete) dict {
   def w = _gf2_dense_width(rows, width)
   def packed_row_word_ops = rows.len * _gf2_word_count(w)
   def possible_post_lanczos_rows_saved = _gf2_post_lanczos_prefix_rows(rows.len, w, true)
   def sparse_probe = _gf2_sparse_from_dense_rows_until(rows, packed_row_word_ops, possible_post_lanczos_rows_saved, w)
   def sparse_entries = int(sparse_probe.get("entries", 0))
   def use_sparse_kernel = sparse_entries > 0 && !bool(sparse_probe.get("overflow", false))
   def sparse_rows = use_sparse_kernel ? sparse_probe.get("rows", []) : []
   def packed_rows = use_sparse_kernel ? [] : _gf2_pack_rows(rows, w, false).get(0)
   def post_lanczos_rows_saved = _gf2_post_lanczos_prefix_rows(rows.len, w, use_sparse_kernel)
   def post_lanczos_enabled = post_lanczos_rows_saved > 0
   def sparse_work_rows = post_lanczos_enabled ? sparse_probe.get("work_rows", []) : sparse_rows
   def sparse_work_entries = use_sparse_kernel ? int(sparse_probe.get("work_entries", sparse_entries)) : 0
   def sparse_work_word_terms = use_sparse_kernel ? int(sparse_probe.get("work_word_terms", 0)) : 0
   def plan = _gf2_block_krylov_iteration_plan(w, block_size, max_iters, complete)
   _dict_with(18, [
         ["width", w], ["packed_rows", packed_rows],
         ["sparse_rows", sparse_rows], ["sparse_entries", sparse_entries],
         ["packed_row_word_ops", packed_row_word_ops],
         ["use_sparse_kernel", use_sparse_kernel],
         ["post_lanczos_rows_saved", post_lanczos_rows_saved],
         ["post_lanczos_enabled", post_lanczos_enabled],
         ["sparse_work_rows", sparse_work_rows],
         ["sparse_work_entries", sparse_work_entries],
         ["sparse_work_word_rows", []],
         ["sparse_work_word_terms", sparse_work_word_terms],
         ["sparse_work_word_entries", sparse_work_entries],
         ["post_lanczos_saved_entries", use_sparse_kernel ? sparse_entries - sparse_work_entries : 0],
         ["post_lanczos_saved_row_words", post_lanczos_enabled ? _gf2_word_count(w) : 0],
         ["block_size", plan.get("block_size")],
         ["iters", plan.get("iters")],
         ["bounded_default_iters", plan.get("bounded_default_iters")],
      ])
}

fn _gf2_block_krylov_report_fields(str vector_count_key, list rows, dict setup, dict walk, dict image_report, dict solution, int vector_count) list {
   def w = int(setup.get("width", 0))
   def bs = int(setup.get("block_size", 0))
   def iters = int(setup.get("iters", 0))
   def post_rows = int(setup.get("post_lanczos_rows_saved", 0))
   def saved_row_words = int(setup.get("post_lanczos_saved_row_words", 0))
   def use_sparse_kernel = bool(setup.get("use_sparse_kernel", false))
   mut fields = _gf2_block_krylov_core_fields(vector_count_key, use_sparse_kernel, rows, setup.get("sparse_work_rows", []), w, bs, iters, bool(setup.get("bounded_default_iters", false)), vector_count, bool(image_report.get("packed_transpose", false)), int(image_report.get("packed_row_words", 0)), int(setup.get("sparse_entries", 0)), int(setup.get("packed_row_word_ops", 0)))
   fields = _fields_extend(fields, _gf2_block_krylov_post_lanczos_fields(bool(setup.get("post_lanczos_enabled", false)), post_rows, use_sparse_kernel ? setup.get("sparse_work_rows", []).len : rows.len, int(setup.get("post_lanczos_saved_entries", 0)), int(setup.get("sparse_work_entries", 0)), saved_row_words, post_rows * saved_row_words, int(walk.get("matvecs", 0))))
   fields = _fields_extend(fields, _gf2_block_krylov_op_fields_from(w, walk, int(solution.get("packed_vector_decode_ops", 0))))
   fields = _fields_extend(fields, [
         ["sparse_packed_matvec_enabled", walk.get("sparse_packed_matvec_enabled", false)],
         ["sparse_packed_transpose_enabled", walk.get("sparse_packed_transpose_enabled", false)],
         ["sparse_packed_matvecs", walk.get("sparse_packed_matvecs", 0)],
         ["sparse_packed_transpose_matvecs", walk.get("sparse_packed_transpose_matvecs", 0)],
         ["sparse_packed_bit_reads", walk.get("sparse_packed_bit_reads", 0)],
         ["sparse_work_word_terms", setup.get("sparse_work_word_terms", 0)],
         ["sparse_work_word_entries", setup.get("sparse_work_word_entries", 0)],
         ["sparse_word_dot_ops", walk.get("sparse_word_dot_ops", 0)],
         ["sparse_word_transpose_xor_ops", walk.get("sparse_word_transpose_xor_ops", 0)],
         ["sparse_dense_vector_materializations_avoided", walk.get("sparse_dense_vector_materializations_avoided", 0)],
         ["sparse_seen_key_repack_avoided", walk.get("sparse_seen_key_repack_avoided", 0)],
         ["sparse_dual_transpose_enabled", walk.get("sparse_dual_transpose_enabled", false)],
         ["sparse_dual_transpose_matvecs", walk.get("sparse_dual_transpose_matvecs", 0)],
      ])
   _fields_extend(fields, solution.get("fields", []))
}

fn _gf2_block_krylov_report(str method, str vector_count_key, bool shifted_seed, list rows, int width, int block_size, int max_iters, bool complete) dict {
   "Return verified GF(2) nullspace diagnostics using block normal Krylov projections."
   def t0 = ticks()
   def setup = _gf2_block_krylov_setup(rows, width, block_size, max_iters, complete)
   def w = int(setup.get("width", 0))
   def use_sparse_kernel = bool(setup.get("use_sparse_kernel", false))
   def walk = _gf2_block_krylov_walk_report(use_sparse_kernel, shifted_seed, setup.get("packed_rows", []), setup.get("sparse_work_rows", []), int(setup.get("sparse_work_entries", 0)), w, int(setup.get("block_size", 8)), int(setup.get("iters", 0)))
   def vectors = walk.get("vectors", [])
   def vectors_packed = walk.get("vectors_packed", [])
   def images = walk.get("images", [])
   def vector_count = vectors_packed.len
   def image_basis_limit = complete ? 0 : (vector_count > 512 ? max(64, int(setup.get("block_size", 8)) * 8) : max(128, int(setup.get("block_size", 8)) * 16))
   def image_report = _gf2_block_krylov_image_report(use_sparse_kernel, images, setup.get("sparse_work_rows", []), rows, vector_count, image_basis_limit)
   def image_la = image_report.get("linear_algebra", dict())
   def deps = image_la.get("basis", [])
   def solution = _gf2_block_krylov_solution_report(use_sparse_kernel, rows, setup.get("sparse_rows", []), w, deps, vector_count, vectors, vectors_packed, image_basis_limit, complete, image_la)
   _finish_report_with(_report(method, 26), t0, _gf2_block_krylov_report_fields(vector_count_key, rows, setup, walk, image_report, solution, vector_count))
}

fn _gf2_block_krylov_sparse_setup(list sparse_rows, int width, int block_size, int max_iters, bool complete) dict {
   def w = width
   def sparse_entries = _gf2_sparse_entry_count(sparse_rows)
   def post_rows = _gf2_post_lanczos_prefix_rows(sparse_rows.len, w, true)
   def post_enabled = post_rows > 0
   def tail = post_enabled ? _gf2_sparse_tail_rows_report(sparse_rows, post_rows) : _dict_with(4, [["rows", sparse_rows], ["entries", sparse_entries]])
   def plan = _gf2_block_krylov_iteration_plan(w, block_size, max_iters, complete)
   _dict_with(18, [
         ["width", w], ["packed_rows", []],
         ["sparse_rows", sparse_rows], ["sparse_entries", sparse_entries],
         ["packed_row_word_ops", sparse_rows.len * _gf2_word_count(w)],
         ["use_sparse_kernel", true],
         ["post_lanczos_rows_saved", post_rows],
         ["post_lanczos_enabled", post_enabled],
         ["sparse_work_rows", tail.get("rows", sparse_rows)],
         ["sparse_work_entries", int(tail.get("entries", sparse_entries))],
         ["sparse_work_word_rows", []],
         ["sparse_work_word_terms", int(tail.get("entries", sparse_entries))],
         ["sparse_work_word_entries", int(tail.get("entries", sparse_entries))],
         ["post_lanczos_saved_entries", sparse_entries - int(tail.get("entries", sparse_entries))],
         ["post_lanczos_saved_row_words", post_enabled ? _gf2_word_count(w) : 0],
         ["block_size", plan.get("block_size")],
         ["iters", plan.get("iters")],
         ["bounded_default_iters", plan.get("bounded_default_iters")],
      ])
}

fn _gf2_block_krylov_sparse_report(str method, str vector_count_key, bool shifted_seed, list sparse_rows, int width, int block_size, int max_iters) dict {
   def t0 = ticks()
   def setup = _gf2_block_krylov_sparse_setup(sparse_rows, width, block_size, max_iters, false)
   def w = int(setup.get("width", 0))
   def work_rows = setup.get("sparse_work_rows", [])
   def walk = _gf2_block_krylov_walk_report(true, shifted_seed, [], work_rows, int(setup.get("sparse_work_entries", 0)), w, int(setup.get("block_size", 8)), int(setup.get("iters", 0)))
   def vectors_packed = walk.get("vectors_packed", [])
   def images = walk.get("images", [])
   def vector_count = vectors_packed.len
   def image_basis_limit = vector_count > 512 ? max(64, int(setup.get("block_size", 8)) * 8) : max(128, int(setup.get("block_size", 8)) * 16)
   def image_report = _gf2_block_krylov_image_report(true, images, work_rows, [], vector_count, image_basis_limit)
   def image_la = image_report.get("linear_algebra", dict())
   def solution = _gf2_block_krylov_solution_report(true, sparse_rows, sparse_rows, w, image_la.get("basis", []), vector_count, [], vectors_packed, image_basis_limit, false, image_la)
   _finish_report_with(_report(method, 26), t0, _gf2_block_krylov_report_fields(vector_count_key, sparse_rows, setup, walk, image_report, solution, vector_count))
}

fn block_lanczos_gf2_report(list rows, int width=0, int block_size=8, int max_iters=0, bool complete=true) dict {
   "Return verified GF(2) nullspace diagnostics using block Krylov/Lanczos-style projections."
   _gf2_block_krylov_report("block-lanczos-gf2", "krylov_vectors", false, rows, width, block_size, max_iters, complete)
}

fn block_lanczos_gf2_nullspace(list rows, int width=0, int block_size=8, int max_iters=0, bool complete=true) list {
   "Return a verified GF(2) nullspace basis from block_lanczos_gf2_report."
   block_lanczos_gf2_report(rows, width, block_size, max_iters, complete).get("basis", [])
}

fn block_wiedemann_gf2_report(list rows, int width=0, int block_size=8, int max_iters=0, bool complete=true) dict {
   "Return verified GF(2) nullspace diagnostics using Wiedemann-style normal Krylov sequences."
   _gf2_block_krylov_report("block-wiedemann-gf2", "sequence_vectors", true, rows, width, block_size, max_iters, complete)
}

fn block_wiedemann_gf2_nullspace(list rows, int width=0, int block_size=8, int max_iters=0, bool complete=true) list {
   "Return a verified GF(2) nullspace basis from block_wiedemann_gf2_report."
   block_wiedemann_gf2_report(rows, width, block_size, max_iters, complete).get("basis", [])
}

fn _gf2_dependency_candidates(list basis, int width, int max_count=4096) list {
   if basis.len == 0 { return [] }
   if basis.len > 14 { return basis }
   def limit = 1 << basis.len
   def out_count = min(max_count, max(0, limit - 1))
   mut out = list(out_count)
   __list_set_len(out, out_count)
   mut mask = 1
   mut out_i = 0
   while mask < limit && out_i < out_count {
      mut dep = _gf2_zero_vec(width)
      mut i = 0
      while i < basis.len {
         if ((mask >> i) & 1) == 1 { dep = _gf2_xor_vec(dep, basis.get(i)) }
         i += 1
      }
      out[out_i] = dep
      out_i += 1
      mask += 1
   }
   out
}

fn gf2_dependency_candidates_report(list basis, int width=0, int max_count=4096, bool include_weights=true) dict {
   "Return all non-empty GF(2) combinations of a nullspace basis, capped for solver probes."
   def t0 = ticks()
   mut w = width
   if w <= 0 && basis.len > 0 { w = _gf2_dense_width([basis.get(0)], 0) }
   mut cap = max_count
   if cap <= 0 { cap = 4096 }
   def large_passthrough = basis.len > 14
   def expected = large_passthrough ? basis.len : ((1 << basis.len) - 1)
   def candidates = _gf2_dependency_candidates(basis, w, cap)
   mut weights = include_weights ? list(candidates.len) : []
   if include_weights { __list_set_len(weights, candidates.len) }
   if include_weights {
      mut i = 0
      while i < candidates.len {
         weights[i] = _gf2_vec_weight(candidates.get(i))
         i += 1
      }
   }
   _report_with("gf2-dependency-candidates", t0, [
         ["basis_count", basis.len], ["width", w], ["max_count", cap],
         ["expected_combination_count", expected],
         ["large_basis_passthrough", large_passthrough],
         ["candidate_count", candidates.len], ["truncated", candidates.len < expected],
         ["candidates", candidates], ["candidate_weights", weights],
      ])
}

fn gf2_dependency_candidates(list basis, int width=0, int max_count=4096) list {
   "Return dependency combinations from gf2_dependency_candidates_report."
   gf2_dependency_candidates_report(basis, width, max_count).get("candidates", [])
}

fn _gf2_xor_vec_into(list out, list rhs, int width) any {
   mut i = 0
   while i < width && i < out.len && i < rhs.len {
      out[i] = (int(out[i]) ^^ int(rhs[i])) & 1
      i += 1
   }
   nil
}

fn _gf2_expand_precondition_dependency(list dep, any precondition, int original_width) list {
   if precondition == nil { return dep }
   def cycles = precondition.get("col_cycles", [])
   if cycles.len == 0 { return dep }
   mut out = _gf2_zero_vec(original_width)
   mut j = 0
   while j < dep.len && j < cycles.len {
      if (int(dep.get(j, 0)) & 1) != 0 {
         def cyc = cycles.get(j)
         mut k = 0
         while k < cyc.len {
            def idx = int(cyc.get(k))
            if idx >= 0 && idx < original_width { out[idx] = int(out.get(idx, 0)) ^^ 1 }
            k += 1
         }
      }
      j += 1
   }
   out
}

fn _gf2_pipeline_precondition(list rows, int width, bool enabled) dict {
   mut solve_matrix = rows
   mut solve_width = width
   mut precondition = nil
   if enabled {
      def skip_shape = rows.len >= 256 && width >= rows.len * 2
      if !skip_shape && !_gf2_precondition_quick_noop(rows, width, true, true) {
         precondition = gf2_matrix_precondition_report(rows, width, 8, true, false, true)
         def reduced_cols = int(precondition.get("output_cols", width))
         def no_effect = reduced_cols == width
         && int(precondition.get("output_rows", rows.len)) == rows.len
         && int(precondition.get("empty_columns_removed", 0)) == 0
         && int(precondition.get("singleton_columns_removed", 0)) == 0
         && int(precondition.get("clique_merges", 0)) == 0
         && int(precondition.get("trimmed_heavy_columns", 0)) == 0
         && int(precondition.get("immediate_dependency_count", 0)) == 0
         if no_effect { precondition = nil }
         if reduced_cols >= 0 && reduced_cols < width {
            solve_matrix = precondition.get("reduced_rows", rows)
            solve_width = reduced_cols
         }
      }
   }
   _dict_with(6, [
         ["solve_matrix", solve_matrix],
         ["solve_width", solve_width],
         ["precondition", precondition],
      ])
}

fn _gf2_pipeline_empty_linear_algebra() dict {
   _set_fields(_report("empty-gf2-linear-algebra", 8), [
         ["basis", []], ["verified_count", 0], ["matrix_kernel", ""],
      ])
}

fn _gf2_pipeline_linear_algebra(list solve_matrix, int solve_width, str backend, int block_size, int max_iters) dict {
   if solve_width <= 0 { return _gf2_pipeline_empty_linear_algebra() }
   if backend == "sparse" {
      return sparse_gf2_nullspace_report(_gf2_sparse_from_dense_rows(solve_matrix), solve_width)
   }
   if backend == "wiedemann" || backend == "block-wiedemann" {
      return block_wiedemann_gf2_report(solve_matrix, solve_width, block_size, max_iters, false)
   }
   if backend == "lanczos" || backend == "block-lanczos" {
      return block_lanczos_gf2_report(solve_matrix, solve_width, block_size, max_iters, false)
   }
   packed_gf2_nullspace_report(solve_matrix, solve_width, false)
}

fn _gf2_pipeline_expand_candidates(dict lin, any precondition, int width, int max_candidates) dict {
   mut expanded = []
   def immediate = precondition == nil ? [] : precondition.get("immediate_dependencies", [])
   mut i = 0
   while i < immediate.len && expanded.len < max_candidates {
      expanded = expanded.append(immediate.get(i))
      i += 1
   }
   def raw_basis = lin.get("basis", [])
   i = 0
   while i < raw_basis.len && expanded.len < max_candidates {
      expanded = expanded.append(_gf2_expand_precondition_dependency(raw_basis.get(i), precondition, width))
      i += 1
   }
   _dict_with(6, [
         ["expanded", expanded],
         ["immediate", immediate],
         ["raw_basis", raw_basis],
      ])
}

fn _gf2_pipeline_verify(list rows, int width, list expanded) dict {
   def sparse_rows = _gf2_sparse_from_dense_rows(rows)
   def sparse_entries = _gf2_sparse_entry_count(sparse_rows)
   def packed_row_word_ops = rows.len * _gf2_word_count(width)
   sparse_entries > 0 && sparse_entries <= packed_row_word_ops ? _gf2_verified_sparse_basis_report(sparse_rows, expanded, width) : _gf2_verified_basis_report(rows, expanded, width)
}

fn _gf2_pipeline_exact_closure(list rows, int width, list basis_in, dict verified, bool complete) dict {
   mut basis = basis_in
   mut exact = dict()
   mut exact_added = 0
   mut exact_nullity = -1
   if complete {
      exact = packed_gf2_nullspace_report(rows, width, false)
      exact_nullity = exact.get("nullity", 0)
      def closure = _gf2_independent_basis_report(_list_concat(basis, exact.get("basis", [])), width)
      basis = closure.get("basis", basis)
      exact_added = basis.len - verified.get("rank", basis.len)
   }
   _dict_with(6, [
         ["basis", basis],
         ["exact", exact],
         ["exact_added", exact_added],
         ["exact_nullity", exact_nullity],
      ])
}

fn _gf2_pipeline_immediate_linear_algebra(dict lin, list basis, list immediate, dict verified, int solve_width) dict {
   if solve_width <= 0 && immediate.len > 0 {
      return _set_fields(_report("precondition-immediate-gf2", 12), [
            ["basis", basis],
            ["verified_count", basis.len],
            ["matrix_kernel", "precondition-immediate"],
            ["precondition_only", true],
            ["immediate_dependency_count", immediate.len],
            ["verified_basis_report", verified],
         ])
   }
   lin
}

fn _gf2_pipeline_fields(
   str backend, list rows, int width, dict prepared, bool precondition_enabled,
   dict lin, dict expanded_report, dict verified, dict exact_report, bool complete,
) list {
   def basis = exact_report.get("basis", [])
   def exact_nullity = int(exact_report.get("exact_nullity", -1))
   [
      ["backend", backend], ["input_rows", rows.len], ["input_cols", width],
      ["solve_rows", prepared.get("solve_matrix", rows).len], ["solve_cols", prepared.get("solve_width", width)],
      ["precondition_enabled", precondition_enabled], ["matrix_precondition", prepared.get("precondition", nil)],
      ["linear_algebra", lin], ["matrix_kernel", lin.get("matrix_kernel", "")],
      ["immediate_candidate_count", expanded_report.get("immediate", []).len],
      ["raw_basis_count", expanded_report.get("raw_basis", []).len],
      ["expanded_candidate_count", expanded_report.get("expanded", []).len],
      ["verified_basis_report", verified],
      ["verification_matrix_kernel", verified.get("matrix_kernel", "dense-row-scan")],
      ["basis", basis], ["verified_count", basis.len],
      ["complete", complete && exact_nullity >= 0 && basis.len == exact_nullity],
      ["exact_nullity", exact_nullity],
      ["exact_added", exact_report.get("exact_added", 0)],
      ["exact_linear_algebra", exact_report.get("exact", dict())],
   ]
}

fn gf2_dependency_pipeline_report(
   list rows, int width=0, str backend="lanczos",
   bool precondition_enabled=true, int block_size=8, int max_iters=0,
   bool complete=false, int max_candidates=8192,
) dict {
   "Return verified original-column GF(2) dependencies with preconditioning and expansion reports."
   def t0 = ticks()
   def w = _gf2_dense_width(rows, width)
   def prepared = _gf2_pipeline_precondition(rows, w, precondition_enabled)
   mut lin = _gf2_pipeline_linear_algebra(prepared.get("solve_matrix", rows), int(prepared.get("solve_width", w)), backend, block_size, max_iters)
   def precondition = prepared.get("precondition", nil)
   def expanded_report = _gf2_pipeline_expand_candidates(lin, prepared.get("precondition", nil), w, max_candidates)
   def expanded = expanded_report.get("expanded", [])
   def raw_basis = expanded_report.get("raw_basis", [])
   mut verified = dict()
   if precondition == nil && expanded.len == raw_basis.len && lin.get("verified_basis_report", nil) != nil {
      verified = lin.get("verified_basis_report")
   } else {
      verified = _gf2_pipeline_verify(rows, w, expanded)
   }
   def exact_report = _gf2_pipeline_exact_closure(rows, w, verified.get("basis", []), verified, complete)
   lin = _gf2_pipeline_immediate_linear_algebra(lin, exact_report.get("basis", []), expanded_report.get("immediate", []), verified, int(prepared.get("solve_width", w)))
   _report_with("gf2-dependency-pipeline", t0, _gf2_pipeline_fields(backend, rows, w, prepared, precondition_enabled, lin, expanded_report, verified, exact_report, complete))
}

fn _gf2_dependency_sparse_pipeline_report(list sparse_rows, int width, str backend="lanczos", int block_size=8, int max_iters=0, int max_candidates=8192) dict {
   def t0 = ticks()
   mut lin = dict()
   if backend == "wiedemann" || backend == "block-wiedemann" {
      lin = _gf2_block_krylov_sparse_report("block-wiedemann-gf2", "sequence_vectors", true, sparse_rows, width, block_size, max_iters)
   } else {
      lin = _gf2_block_krylov_sparse_report("block-lanczos-gf2", "krylov_vectors", false, sparse_rows, width, block_size, max_iters)
   }
   def basis = lin.get("basis", [])
   def cap = max_candidates <= 0 ? basis.len : min(max_candidates, basis.len)
   mut expanded = list(cap)
   __list_set_len(expanded, cap)
   mut i = 0
   while i < cap {
      expanded[i] = basis[i]
      i += 1
   }
   def verified = lin.get("verified_basis_report", _gf2_verified_sparse_basis_report(sparse_rows, expanded, width))
   def exact_report = _gf2_pipeline_exact_closure(sparse_rows, width, verified.get("basis", []), verified, false)
   lin = _gf2_pipeline_immediate_linear_algebra(lin, exact_report.get("basis", []), [], verified, width)
   _report_with("gf2-dependency-pipeline", t0, [
         ["backend", backend], ["rows", sparse_rows.len], ["cols", width],
         ["input_rows", sparse_rows.len], ["input_cols", width],
         ["solve_rows", sparse_rows.len], ["solve_cols", width],
         ["precondition_enabled", false], ["matrix_precondition", nil],
         ["linear_algebra", lin], ["basis", exact_report.get("basis", verified.get("basis", []))],
         ["dependency_count", exact_report.get("basis", verified.get("basis", [])).len],
         ["verified_count", int(verified.get("verified_count", 0))],
         ["verified_basis_report", verified],
         ["exact_closure_report", exact_report],
         ["complete", false],
      ])
}
