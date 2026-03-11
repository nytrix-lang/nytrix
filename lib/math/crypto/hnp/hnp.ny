;; Keywords: hnp hidden-number-problem
;; Hidden number problem operations.
;; Lattice-based partial nonce recovery for ECDSA/DLP
;; Reference:
;; - https://crypto.stanford.edu/~dabo/pubs/papers/dhmsb.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
module std.math.crypto.hnp.hnp(hnp_centered_mod, hnp_leak_bound, hnp_embedding, hnp_default_sample_counts, hnp_partition_samples, hnp_bound_constraint, hnp_replace_samples, hnp_prescreen_target, hnp_lsb_lattice_eliminate_alpha, hnp_lsb_lattice_increase_volume, hnp_alpha_from_target, hnp_check_alpha, hnp_linear_predicate_eliminate_alpha, hnp_recover, hnp_lattice, hnp_recover_x, hnp_extract_x)
use std.math.nt
use std.math.matrix as matrix
use std.math.crypto.lattice.lll as lllmod

fn hnp_centered_mod(any: value, any: modulus): bigint {
   "Return value modulo modulus in the centered interval(-modulus/2, modulus/2]."
   def q = Z(modulus)
   mut v = Z(value) % q
   if(v > q / Z(2)){ v = v - q }
   v
}

fn hnp_leak_bound(any: modulus, any: leaked_bits): bigint {
   "Return the half-width q / 2^(s+1) used by nonce-leak HNP predicates."
   def denom = Z(1) << int(leaked_bits + 1)
   Z(modulus) / denom
}

fn hnp_embedding(any: modulus, any: leaked_bits): bigint {
   "Default integer embedding scale, approximating q / (2^(s+1) * sqrt(3))."
   def bound = hnp_leak_bound(modulus, leaked_bits)
   def e = (bound * Z(577)) / Z(1000)
   e <= 0 ? Z(1) : e
}

fn hnp_default_sample_counts(any: bit_size, any: lattice_samples, any: volume_bits=0): list {
   "Return [lattice, reduce, narrow, check] sample counts for the leakage solver."
   [int(lattice_samples), 2 * int(volume_bits), int(volume_bits), 2 * int(bit_size)]
}

fn hnp_partition_samples(list: samples, list: counts): list {
   "Split samples into [lattice, reduce, narrow, check] groups according to counts."
   mut groups = [[], [], [], []]
   mut pos = 0
   mut g = 0
   while(g < 4){
      mut bucket = []
      mut i = 0
      def want = int(counts.get(g, 0))
      while(i < want && pos < samples.len){
         bucket = bucket.append(samples[pos])
         pos += 1
         i += 1
      }
      groups[g] = bucket
      g += 1
   }
   groups
}

fn _hnp_zero_row(int: n): list {
   mut row = []
   mut i = 0
   while(i < n){
      row = row.append(Z(0))
      i += 1
   }
   row
}

fn _hnp_abs(any: x): any { x < 0 ? -x : x }

fn hnp_bound_constraint(any: first_t, any: sample_t, any: modulus, any: bound): bool {
   "Return true when t0^-1 * sample_t is centered inside +/- bound modulo modulus."
   def q = Z(modulus)
   _hnp_abs(hnp_centered_mod(inverse_mod(Z(first_t) % q, q) * Z(sample_t), q)) <= Z(bound)
}

fn hnp_replace_samples(list: samples, any: modulus, any: leaked_bits, str: mode="shifted"): list {
   "Replace HNP samples against the first sample.
   `shifted` mode uses(a_i+B)-t_i'(a_0+B), matching lattice/prescreen rows.
   `raw` mode uses a_i-a_0*t_i', matching interval narrowing rows."
   if(samples.len < 2){ return [] }
   def q = Z(modulus)
   def bound = hnp_leak_bound(q, leaked_bits)
   def first = samples[0]
   def t0_inv = inverse_mod(Z(first[0]) % q, q)
   def a0 = Z(first[1])
   mut out = []
   mut i = 1
   while(i < samples.len){
      def sample = samples[i]
      def t_rep = (t0_inv * Z(sample[0])) % q
      def a_rep = mode == "raw" ? (Z(sample[1]) - t_rep * a0) % q : ((Z(sample[1]) + bound) - t_rep * (a0 + bound)) % q
      out = out.append([t_rep, a_rep])
      i += 1
   }
   out
}

fn hnp_prescreen_target(list: replaced_samples, any: target, any: modulus, any: bound, int: max_errors=0): bool {
   "Fast linear predicate pre-screen for a candidate target against replaced samples."
   def q, b = Z(modulus), Z(bound)
   mut errors = 0
   mut i = 0
   while(i < replaced_samples.len){
      def sample = replaced_samples[i]
      def residue = ((Z(sample[0]) * Z(target) - Z(sample[1]) + b) % q) - b
      if(_hnp_abs(residue) > b){
         errors += 1
         if(errors > max_errors){ return false }
      }
      i += 1
   }
   true
}

fn _hnp_lsb_lattice_rows(list: samples, any: modulus, any: leaked_bits, any: volume_bits=0, any: embedding=nil): any {
   def n = samples.len
   if(n < 2){ return nil }
   def q = Z(modulus)
   def emb = embedding == nil ? hnp_embedding(q, leaked_bits) : Z(embedding)
   def scale = Z(1) << int(volume_bits)
   def dim = n + 1
   def replaced = hnp_replace_samples(samples, q, leaked_bits)
   mut rows = []
   mut i = 0
   while(i < dim){
      rows = rows.append(_hnp_zero_row(dim))
      i += 1
   }
   i = 0
   while(i < n - 1){
      mut row = rows[i]
      row[i] = q
      rows[i] = row
      i += 1
   }
   i = 0
   while(i < replaced.len){
      def sample = replaced[i]
      mut trow = rows[n - 1]
      trow[i] = sample[0] * scale
      rows[n - 1] = trow
      mut arow = rows[n]
      arow[i] = sample[1]
      rows[n] = arow
      i += 1
   }
   mut tlast = rows[n - 1]
   tlast[n - 1] = scale
   rows[n - 1] = tlast
   mut alast = rows[n]
   alast[n] = emb
   rows[n] = alast
   rows
}

fn hnp_lsb_lattice_eliminate_alpha(list: samples, any: modulus, any: leaked_bits, any: embedding=nil): any {
   "Build the AH21-style HNP lattice after eliminating alpha with the first sample.
   `samples` is a list of [t, a] pairs satisfying t*alpha - a = small(mod q)."
   def rows = _hnp_lsb_lattice_rows(samples, modulus, leaked_bits, 0, embedding)
   rows == nil ? nil : matrix.Matrix(rows)
}

fn hnp_lsb_lattice_increase_volume(list: samples, any: modulus, any: leaked_bits, any: volume_bits=0, any: embedding=nil): any {
   "Build the increased-volume HNP lattice for partial-nonce datasets.
   `volume_bits` is the x parameter used to scale the t row by 2^x."
   def rows = _hnp_lsb_lattice_rows(samples, modulus, leaked_bits, volume_bits, embedding)
   rows == nil ? nil : matrix.Matrix(rows)
}

fn hnp_alpha_from_target(any: first_t, any: first_a, any: target, any: tau, any: modulus, any: leaked_bits, any: embedding=nil): any {
   "Recover alpha from an HNP target vector coordinate and embedding sign.
   Returns nil when the target does not satisfy the linear predicate shape."
   def q = Z(modulus)
   def bound = hnp_leak_bound(q, leaked_bits)
   def emb = embedding == nil ? hnp_embedding(q, leaked_bits) : Z(embedding)
   def t = Z(target)
   def tv = Z(tau)
   def k0 = (t == 0 || _hnp_abs(t) > bound) ? nil : ((tv == emb) ? bound - t : ((tv == -emb) ? t + bound : nil))
   k0 == nil ? nil : inverse_mod(Z(first_t) % q, q) * ((Z(first_a) + k0) % q) % q
}

fn hnp_check_alpha(any: alpha, list: samples, any: modulus, any: leaked_bits, int: max_errors=0): bool {
   "Check an HNP alpha candidate against samples using the linear predicate bound."
   def q = Z(modulus)
   def bound = hnp_leak_bound(q, leaked_bits)
   mut errors = 0
   mut i = 0
   while(i < samples.len){
      def sample = samples[i]
      if(((Z(alpha) * Z(sample[0]) - Z(sample[1])) % q) > Z(2) * bound){
         errors += 1
         if(errors > max_errors){ return false }
      }
      i += 1
   }
   true
}

fn hnp_linear_predicate_eliminate_alpha(list: first, any: target, any: tau, list: checks, any: q, any: bits, int: max_errors=0, any: embedding=nil): any {
   "Apply the eliminate-alpha linear predicate and return alpha or nil."
   def alpha = hnp_alpha_from_target(first[0], first[1], target, tau, q, bits, embedding)
   alpha == nil ? nil : (hnp_check_alpha(alpha, checks, q, bits, max_errors) ? alpha : nil)
}

fn _hnp_slice(list: xs, int: start, int: count): list {
   mut out = []
   mut i = 0
   while(i < count && start + i < xs.len){
      out = out.append(xs[start + i])
      i += 1
   }
   out
}

fn _hnp_count_errors(any: alpha, list: samples, any: modulus, any: leaked_bits): int {
   def q = Z(modulus)
   def bound = hnp_leak_bound(q, leaked_bits)
   mut errors = 0
   mut i = 0
   while(i < samples.len){
      def sample = samples[i]
      if(((Z(alpha) * Z(sample[0]) - Z(sample[1])) % q) > Z(2) * bound){ errors += 1 }
      i += 1
   }
   errors
}

fn _hnp_dict(list: pairs): dict {
   mut out = dict(pairs.len)
   mut i = 0
   while(i < pairs.len){
      def pair = pairs[i]
      out = out.set(pair[0], pair[1])
      i += 1
   }
   out
}

fn _hnp_result(
   bool: ok,
   any: alpha,
   str: reason,
   any: samples,
   any: lattice_samples,
   any: checks,
   str: method,
   int: volume_bits,
   any: row_index=nil,
   any: target=nil,
   any: tau=nil,
   any: errors=nil,
): dict {
   _hnp_dict([
         ["ok", ok], ["alpha", alpha], ["key", alpha], ["reason", reason],
         ["samples", samples], ["lattice_samples", lattice_samples],
         ["checks", checks], ["method", method], ["volume_bits", volume_bits],
         ["row", row_index], ["target", target], ["tau", tau], ["errors", errors],
   ])
}

fn _hnp_opt(dict: opts, str: key, any: fallback): any { opts.get(key, fallback) }

fn _hnp_opt_int(dict: opts, str: key, any: fallback): int { int(_hnp_opt(opts, key, fallback)) }

fn _hnp_recover_opts(any: opts, int: sample_len): list {
   "Normalize hnp_recover option dict into [method, delta, max_errors, volume_bits, embedding, counts, lattice_samples]."
   def o = is_dict(opts) ? opts : dict(0)
   def volume_bits = _hnp_opt_int(o, "volume_bits", _hnp_opt(o, "x", 0))
   mut want = _hnp_opt_int(o, "lattice_samples", sample_len)
   want = want < 2 ? 2 : want
   want = want > sample_len ? sample_len : want
   [
      _hnp_opt(o, "method", "pure"),
      _hnp_opt(o, "delta", 0.75),
      _hnp_opt_int(o, "max_errors", 0),
      volume_bits,
      _hnp_opt(o, "embedding", nil),
      _hnp_opt(o, "counts", nil),
      want,
   ]
}

fn _hnp_try_candidate(list: first, list: checks, any: q, any: bits, any: target, any: tau, int: max_errors, any: embedding): any {
   def alpha = hnp_alpha_from_target(first[0], first[1], target, tau, q, bits, embedding)
   alpha == nil ? nil : (hnp_check_alpha(alpha, checks, q, bits, max_errors) ? alpha : nil)
}

fn _hnp_try_row(any: row, list: first, list: checks, any: q, any: bits, int: max_errors, any: embedding, int: volume_bits): any {
   if(!is_list(row) || row.len < 2){ return nil }
   def target_idx = row.len - 2
   def tau_idx = row.len - 1
   mut target = Z(row[target_idx])
   mut tau = Z(row[tau_idx])
   if(volume_bits <= 0){
      def alpha = _hnp_try_candidate(first, checks, q, bits, target, tau, max_errors, embedding)
      return alpha == nil ? nil : [alpha, target, tau]
   }
   def emb = embedding == nil ? hnp_embedding(q, bits) : Z(embedding)
   def invalid = target == 0 || _hnp_abs(tau) != emb
   def flip = tau == emb
   tau = flip ? -tau : tau
   target = flip ? -target : target
   def radius = Z(1) << max(int(volume_bits) - 1, 0)
   mut cand = invalid ? target + radius : target - radius
   def stop = target + radius
   mut hit = nil
   while(hit == nil && cand < stop){
      def alpha = _hnp_try_candidate(first, checks, q, bits, cand, tau, max_errors, embedding)
      hit = alpha == nil ? nil : [alpha, cand, tau]
      cand += Z(1)
   }
   hit
}

fn hnp_recover(any: samples, any: modulus, any: leaked_bits, any: opts=nil): dict {
   "Recover an HNP hidden number from [t,a] samples.
   Options dict: method(pure/auto/lll), volume_bits or x, lattice_samples,
   counts, embedding, max_errors, delta. Returns a result dict with `ok` and `key`."
   if(!is_list(samples) || samples.len < 2){ return _hnp_result(false, nil, "need at least two samples", is_list(samples) ? samples.len : 0, 0, 0, "auto", 0) }
   def q = Z(modulus)
   def cfg = _hnp_recover_opts(opts, samples.len)
   def method = cfg[0]
   def delta = cfg[1]
   def max_errors = cfg[2]
   def volume_bits = cfg[3]
   def embedding = cfg[4]
   def counts = cfg[5]
   def use_counts = is_list(counts)
   def lattice_samples = use_counts ? hnp_partition_samples(samples, counts)[0] : _hnp_slice(samples, 0, cfg[6])
   if(lattice_samples.len < 2){
      return _hnp_result(false, nil, "need at least two lattice samples",
      samples.len, lattice_samples.len, samples.len, method, volume_bits)
   }
   def basis = volume_bits > 0 ? hnp_lsb_lattice_increase_volume(lattice_samples, q, leaked_bits, volume_bits, embedding) :
   hnp_lsb_lattice_eliminate_alpha(lattice_samples, q, leaked_bits, embedding)
   def reduction_report = basis == nil ? nil : lllmod.lll_reduce_report(basis, delta, method)
   def reduced = reduction_report == nil ? nil : reduction_report.get("basis")
   def failure = basis == nil ? "lattice construction failed" : (reduced == nil ? "lattice reduction failed" : nil)
   if(failure != nil){
      return _hnp_result(false, nil, failure,
      samples.len, lattice_samples.len, samples.len, method, volume_bits).set("lattice_report", reduction_report)
   }
   def rows = reduced[2]
   mut i = 0
   while(i < rows.len){
      def hit = _hnp_try_row(rows[i], lattice_samples[0], samples, q, leaked_bits, max_errors, embedding, volume_bits)
      if(hit != nil){
         def alpha = hit[0]
         def errors = _hnp_count_errors(alpha, samples, q, leaked_bits)
         return _hnp_result(true, alpha, "ok", samples.len, lattice_samples.len, samples.len, method, volume_bits,
         i, hit[1], hit[2], errors).set("lattice_report", reduction_report)
      }
      i += 1
   }
   _hnp_result(false, nil, "no candidate", samples.len, lattice_samples.len, samples.len, method, volume_bits).set("lattice_report", reduction_report)
}

fn hnp_lattice(list: samples, any: modulus, any: known_bits): any {
   "Construct and reduce an HNP lattice, then extract a hidden number candidate."
   def basis = hnp_lsb_lattice_eliminate_alpha(samples, modulus, known_bits)
   if(basis == nil){ return nil }
   def reduced = lllmod.lll(basis)
   hnp_extract_x(reduced, modulus)
}

fn hnp_extract_x(any: reduced, any: modulus): any {
   "Extract a small non-zero coordinate from an LLL-reduced HNP lattice."
   if(reduced == nil){ return nil }
   def nrows = int(reduced[0])
   def ncols = int(reduced[1])
   def data = reduced[2]
   mut i = 0
   while(i < nrows){
      def row = data[i]
      mut j = 0
      while(j < ncols){
         def val = row[j]
         def abs_val = _hnp_abs(val)
         if(abs_val > 0 && abs_val < modulus){ return abs_val }
         j += 1
      }
      i += 1
   }
   nil
}

fn hnp_recover_x(list: samples, any: modulus, any: known_bits): any {
   "Recover the hidden number x from HNP samples."
   hnp_lattice(samples, modulus, known_bits)
}
