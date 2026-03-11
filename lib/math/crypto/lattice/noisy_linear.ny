;; Keywords: lattice noisy-linear
;; Lattice routines for noisy linear relation recovery.
;; Useful when most equations are correct but some bits are flipped.
module std.math.crypto.lattice.noisy_linear(score_gf2_candidate, inlier_mask_gf2, solve_noisy_gf2)
use std.core
use std.math.crypto.gf as gf

fn _bit(any: x): int { int(x) & 1 }

fn _at(any: v, int: i, any: fallback): any {
   if(!is_list(v) || i < 0 || i >= v.len){ return fallback }
   v[i]
}

fn _row_dot_gf2(any: row, list: x): int {
   mut acc = 0
   mut j = 0
   while(j < x.len){
      if(_bit(_at(row, j, 0)) != 0 && _bit(x[j]) != 0){ acc = acc ^^ 1 }
      j += 1
   }
   acc
}

fn _validate_system(any: A, any: b): int {
   panic_if(!is_list(A) || A.len == 0, "solve_noisy_gf2: A must be a non-empty list")
   panic_if(!is_list(b) || b.len != A.len, "solve_noisy_gf2: b must have same length as A")
   def nc = len(A[0])
   panic_if(nc <= 0, "solve_noisy_gf2: A must have at least one column")
   mut i = 1
   while(i < A.len){
      panic_if(len(A[i]) != nc, "solve_noisy_gf2: all rows in A must have equal width")
      i += 1
   }
   nc
}

fn score_gf2_candidate(list: A, list: b, any: x): int {
   "Count how many equations in Ax=b over GF(2) are satisfied by candidate x."
   def usable = is_list(x)
   mut ok = 0
   mut i = 0
   while(i < A.len){
      def lhs = _row_dot_gf2(A[i], x)
      ok += (usable && lhs == _bit(b[i])) ? 1 : 0
      i += 1
   }
   ok
}

fn inlier_mask_gf2(list: A, list: b, list: x): list {
   "Return per-equation inlier mask(1 means satisfied, 0 means mismatch)."
   mut mask = []
   mut i = 0
   while(i < A.len){
      def lhs = _row_dot_gf2(A[i], x)
      mask = mask.append(lhs == _bit(b[i]) ? 1 : 0)
      i += 1
   }
   mask
}

fn _lcg_next(any: state): int {
   ((int(state) * 1103515245 + 12345) & 0x7fffffff)
}

fn _subset_indices(int: nr, int: need, int: seed0): list {
   mut seed = int(seed0)
   mut idxs = []
   mut seen = []
   mut i = 0
   while(i < nr){
      seen = seen.append(0)
      i += 1
   }
   mut tries = 0
   def lim = nr * 8 + 64
   while(idxs.len < need && tries < lim){
      seed = _lcg_next(seed)
      mut idx = seed % nr
      def fresh = seen[idx] == 0
      seen[idx] = fresh ? 1 : seen[idx]
      idxs = fresh ? idxs.append(idx) : idxs
      tries += 1
   }
   i = 0
   while(i < nr && idxs.len < need){
      def fresh = seen[i] == 0
      seen[i] = fresh ? 1 : seen[i]
      idxs = fresh ? idxs.append(i) : idxs
      i += 1
   }
   [idxs, seed]
}

fn _extract_subset(list: A, list: b, list: idxs): list {
   mut As, bs = [], []
   mut i = 0
   while(i < idxs.len){
      def idx = int(idxs[i])
      As, bs = As.append(A[idx]), bs.append(_bit(b[idx]))
      i += 1
   }
   [As, bs]
}

fn _extract_inliers(list: A, list: b, list: mask): list {
   mut As, bs = [], []
   mut i = 0
   while(i < A.len){
      if(int(_at(mask, i, 0)) != 0){ As, bs = As.append(A[i]), bs.append(_bit(b[i])) }
      i += 1
   }
   [As, bs]
}

fn _exact_solution_gf2(list: A, list: b, int: nr): any {
   def exact = gf.solve_gf2(A, b)
   def score = exact == nil ? -1 : score_gf2_candidate(A, b, exact)
   score == nr ? [exact, score, inlier_mask_gf2(A, b, exact)] : nil
}

fn _round_count(int: rounds, int: nr): int { rounds > 0 ? rounds : max(64, nr * 4) }

fn _search_noisy_gf2(list: A, list: b, int: nc, int: nr, int: tries, int: seed0): list {
   mut state = int(seed0)
   mut best_x = nil
   mut best_score = -1
   mut t = 0
   while(t < tries && best_score != nr){
      def picked = _subset_indices(nr, nc, state)
      def idxs = picked[0]
      state = int(picked[1])
      def sub = idxs.len == nc ? _extract_subset(A, b, idxs) : nil
      def cand = sub == nil ? nil : gf.solve_gf2(sub[0], sub[1])
      def sc = cand == nil ? -1 : score_gf2_candidate(A, b, cand)
      def better = sc > best_score
      best_score = better ? sc : best_score
      best_x = better ? cand : best_x
      t += 1
   }
   [best_x, best_score]
}

fn _refit_noisy_gf2(list: A, list: b, any: best_x, int: best_score): list {
   def mask = inlier_mask_gf2(A, b, best_x)
   def in_sys = _extract_inliers(A, b, mask)
   def refit = gf.solve_gf2(in_sys[0], in_sys[1])
   if(refit == nil){ return [best_x, best_score] }
   def score = score_gf2_candidate(A, b, refit)
   score >= best_score ? [refit, score] : [best_x, best_score]
}

fn solve_noisy_gf2(any: A, any: b, int: rounds=0, any: min_inliers=nil, int: seed=1337): any {
   "Recover x from noisy Ax=b over GF(2).
   Returns [x, inlier_count, inlier_mask] or nil."
   def nc, nr = _validate_system(A, b), A.len
   if(nr < nc){ return nil }
   def exact_hit = _exact_solution_gf2(A, b, nr)
   if(exact_hit != nil){ return exact_hit }
   def search = _search_noisy_gf2(A, b, nc, nr, _round_count(rounds, nr), seed)
   mut best_x = search[0]
   mut best_score = int(search[1])
   if(best_x == nil){ return nil }
   def refit = _refit_noisy_gf2(A, b, best_x, best_score)
   best_x = refit[0]
   best_score = int(refit[1])
   if(min_inliers != nil && best_score < int(min_inliers)){ return nil }
   [best_x, best_score, inlier_mask_gf2(A, b, best_x)]
}
