;; Keywords: analysis no-leaks constraints math crypto
;; Cryptanalysis scoring and recovery routines for leak-free candidate filtering and byte recovery.
;; References:
;; - std.math.crypto.analysis
;; - std.math.crypto
module std.math.crypto.analysis.no_leaks(no_leaks_candidates, no_leaks_candidate_sets, no_leaks_apply_sample, no_leaks_recover_bytes, no_leaks_recover_text, no_leaks_summary)
use std.core

fn _no_leaks_byte_at(any sample, int i) int {
   if(sample == nil){ return -1 }
   if(i < 0 || i >= sample.len){ return -1 }
   if(is_str(sample)){ return load8(sample, i) & 255 }
   if(is_list(sample) || is_bytes(sample)){ return sample.get(i) & 255 }
   -1
}

fn _no_leaks_sample_len(any sample) int {
   if(sample == nil){ return 0 }
   if(is_str(sample) || is_list(sample) || is_bytes(sample)){ return sample.len }
   0
}

fn _no_leaks_max_len(list samples) int {
   mut n = 0
   mut i = 0
   while(i < samples.len){
      n = max(n, _no_leaks_sample_len(samples.get(i)))
      i += 1
   }
   n
}

fn _no_leaks_full_row() list {
   mut row = list(256)
   mut b = 0
   while(b < 256){
      row[b] = b
      b += 1
   }
   store64(row, 256, 0)
   row
}

fn _no_leaks_full_flags() list {
   mut row = list(256)
   mut b = 0
   while(b < 256){
      row[b] = 1
      b += 1
   }
   store64(row, 256, 0)
   row
}

fn no_leaks_candidates(int length) list {
   "Build initial candidate byte sets for a no-leaks OTP attack.
   length: target plaintext byte length.
   Returns a list of rows, each row initially containing bytes 0..255."
   mut out = list(length)
   mut i = 0
   while(i < length){
      out[i] = _no_leaks_full_row()
      i += 1
   }
   store64(out, length, 0)
   out
}

fn _no_leaks_without_byte(list row, int blocked) list {
   mut out = list(max(0, row.len - 1))
   mut out_i = 0
   mut i = 0
   while(i < row.len){
      def b = row.get(i) & 255
      if(b != blocked){
         out[out_i] = b
         out_i += 1
      }
      i += 1
   }
   store64(out, out_i, 0)
   out
}

fn no_leaks_apply_sample(list candidates, any sample) list {
   "Apply one accepted ciphertext sample to no-leaks candidate sets.
   In this leak model, an accepted ciphertext byte means the
   plaintext byte at that position is not equal to that byte."
   mut out = list(candidates.len)
   mut i = 0
   while(i < candidates.len){
      def blocked = _no_leaks_byte_at(sample, i)
      def row = candidates.get(i)
      out[i] = blocked >= 0 ? _no_leaks_without_byte(row, blocked) : clone(row)
      i += 1
   }
   store64(out, candidates.len, 0)
   out
}

fn no_leaks_candidate_sets(list samples, int length=0) list {
   "Recover possible plaintext byte sets from accepted no-leaks ciphertexts.
   samples: accepted ciphertexts as byte lists, bytes objects, or strings.
   length: 0 auto-detects the maximum sample length."
   def n = length > 0 ? length : _no_leaks_max_len(samples)
   mut flags = list(n)
   mut i = 0
   while(i < n){
      flags[i] = _no_leaks_full_flags()
      i += 1
   }
   store64(flags, n, 0)
   i = 0
   while(i < samples.len){
      def sample = samples.get(i)
      mut j = 0
      while(j < n){
         def blocked = _no_leaks_byte_at(sample, j)
         if(blocked >= 0){ flags[j][blocked] = 0 }
         j += 1
      }
      i += 1
   }
   mut candidates = list(n)
   i = 0
   while(i < n){
      def row = flags[i]
      mut count = 0
      mut b = 0
      while(b < 256){
         if(row[b] != 0){ count += 1 }
         b += 1
      }
      mut out = list(count)
      mut oi = 0
      b = 0
      while(b < 256){
         if(row[b] != 0){
            out[oi] = b
            oi += 1
         }
         b += 1
      }
      store64(out, count, 0)
      candidates[i] = out
      i += 1
   }
   store64(candidates, n, 0)
   candidates
}

fn _no_leaks_recover_from_candidates(list candidates, int unknown) list {
   mut out = list(candidates.len)
   mut i = 0
   while(i < candidates.len){
      def row = candidates.get(i)
      out[i] = row.len == 1 ? row.get(0) & 255 : unknown
      i += 1
   }
   store64(out, candidates.len, 0)
   out
}

fn _no_leaks_text_from_recovered(list recovered, int fill) str {
   def out = malloc(recovered.len + 1)
   mut i = 0
   while(i < recovered.len){
      def b = recovered.get(i)
      store8(out, b >= 0 ? (b & 255) : fill, i)
      i += 1
   }
   store8(out, 0, recovered.len)
   init_str(out, recovered.len)
}

fn no_leaks_recover_bytes(list samples, int length=0, int unknown=-1) list {
   "Return singleton recovered bytes from no-leaks samples.
   Positions with more than one remaining candidate are filled with unknown."
   _no_leaks_recover_from_candidates(no_leaks_candidate_sets(samples, length), unknown)
}

fn no_leaks_recover_text(list samples, int length=0, str unknown="?") str {
   "Return recovered ASCII text from no-leaks samples.
   Unresolved positions use the first byte of unknown, usually '?'."
   def fill = unknown.len > 0 ? (load8(unknown, 0) & 255) : 63
   _no_leaks_text_from_recovered(no_leaks_recover_bytes(samples, length, -1), fill)
}

fn no_leaks_summary(list samples, int length=0) dict {
   "Summarize a no-leaks OTP recovery session.
   Returns candidates, recovered bytes, text, resolved count, and length."
   def candidates = no_leaks_candidate_sets(samples, length)
   def recovered = _no_leaks_recover_from_candidates(candidates, -1)
   mut resolved = 0
   mut i = 0
   while(i < recovered.len){
      if(recovered.get(i) >= 0){ resolved += 1 }
      i += 1
   }
   {
      "candidates": candidates,
      "bytes": recovered,
      "text": _no_leaks_text_from_recovered(recovered, 63),
      "resolved": resolved,
      "length": recovered.len
   }
}
