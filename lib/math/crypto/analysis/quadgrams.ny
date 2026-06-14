;; Keywords: analysis frequency-analysis language-model quadgrams math crypto
;; Cryptanalysis scoring and recovery routines for quadgram language scoring.
;;
;; Provides a fast scoring function for classical-cipher cracking (hillclimb,
;; substitution, Vigenere key search, etc.). Prefers the repo-local asset profile
;; and falls back to a cached copy of:
;; https://raw.githubusercontent.com/gibsjose/statistical-attack/master/english-quadgrams.txt
;; References:
;; - std.math.crypto.analysis
;; - std.math.crypto
module std.math.crypto.analysis.quadgrams(quadgrams_default_cache_path, quadgrams_load, quadgrams_score, quadgrams_score_upper_ascii)
use std.core
use std.math.scalar (log)
use std.math.crypto.analysis.worldfreq
use std.os (cache_dir, file_exists, file_read, file_write, fetch)
use std.os.path as ospath
use std.core.str as str

def _QUAD_PACKED_SIZE = 456976
mut _quad_loaded = false
mut _quad_packed_loaded = false
mut _quad_logp = 0
mut _quad_logp_packed = 0
mut _quad_floor = 0.0

fn quadgrams_default_cache_path() str {
   "Default on-disk cache location for the quadgram model."
   def cd = cache_dir()
   if cd.len == 0 { return ospath.join(ospath.temp_dir(), "nytrix_crypto_english_quadgrams.txt") }
   ospath.join(cd, "nytrix_crypto_english_quadgrams.txt")
}

fn _quadgrams_ensure_text(str path) str {
   def p = ospath.normalize(path)
   if file_exists(p) { return unwrap(file_read(p)) }
   def repo_p = worldfreq.worldfreq_profile_paths("english").get("quadgrams_path", "")
   if is_str(repo_p) && repo_p.len > 0 && file_exists(repo_p) { return unwrap(file_read(repo_p)) }
   def url = "https://raw.githubusercontent.com/gibsjose/statistical-attack/master/english-quadgrams.txt"
   def body = fetch(url)
   if !is_str(body) || body.len == 0 { panic("quadgrams: fetch failed(missing libcurl?)") }
   unwrap(file_write(p, body))
   body
}

fn _quad_pack4(str s, int i) int {
   (((load8(s, i) - 65) * 26 + load8(s, i + 1) - 65) * 26 + load8(s, i + 2) - 65) * 26 + load8(s, i + 3) - 65
}

fn _quad_alpha_value(int c) int {
   if c >= 65 && c <= 90 { return c - 65 }
   if c >= 97 && c <= 122 { return c - 97 }
   -1
}

fn _quad_filled_table(f64 value) list {
   mut xs = list(_QUAD_PACKED_SIZE)
   mut i = 0
   while i < _QUAD_PACKED_SIZE {
      xs[i] = value
      i += 1
   }
   store64(xs, _QUAD_PACKED_SIZE, 0)
   xs
}

fn _quadgrams_model_path(any path=0) str {
   mut model_path = path
   if !is_str(model_path) || model_path.len == 0 { model_path = quadgrams_default_cache_path() }
   model_path
}

fn _quadgrams_load_packed(any path=0) bool {
   if _quad_packed_loaded { return true }
   def txt = _quadgrams_ensure_text(_quadgrams_model_path(path))
   mut counts = list(_QUAD_PACKED_SIZE)
   mut total = 0.0
   mut i = 0
   def n = txt.len
   while i < n {
      while i < n && (load8(txt, i) == 10 || load8(txt, i) == 13 || load8(txt, i) == 32 || load8(txt, i) == 9) { i += 1 }
      if i + 4 <= n {
         def c0 = load8(txt, i)
         def c1 = load8(txt, i + 1)
         def c2 = load8(txt, i + 2)
         def c3 = load8(txt, i + 3)
         if c0 >= 65 && c0 <= 90 && c1 >= 65 && c1 <= 90 && c2 >= 65 && c2 <= 90 && c3 >= 65 && c3 <= 90 {
            def packed = (((c0 - 65) * 26 + c1 - 65) * 26 + c2 - 65) * 26 + c3 - 65
            i += 4
            while i < n && (load8(txt, i) == 32 || load8(txt, i) == 9) { i += 1 }
            mut cnt_i = 0
            while i < n && load8(txt, i) >= 48 && load8(txt, i) <= 57 {
               cnt_i = cnt_i * 10 + load8(txt, i) - 48
               i += 1
            }
            if cnt_i > 0 {
               counts[packed] = cnt_i
               total = total + float(cnt_i)
            }
         }
      }
      while i < n && load8(txt, i) != 10 { i += 1 }
      i += 1
   }
   if total <= 0.0 { panic("quadgrams: empty model") }
   def floor_log = log(0.01 / total)
   mut m = _quad_filled_table(floor_log)
   i = 0
   while i < _QUAD_PACKED_SIZE {
      def c = int(counts[i])
      if c > 0 { m[i] = log(float(c) / total) }
      i += 1
   }
   _quad_logp_packed = m
   _quad_floor = floor_log
   _quad_packed_loaded = true
   true
}

fn quadgrams_load(any path=0) list {
   "Loads quadgram frequencies into an internal dict of log-probabilities.
   Returns [dict, floor_logp]."
   if _quad_loaded { return [dict_clone(_quad_logp), _quad_floor] }
   def txt = _quadgrams_ensure_text(_quadgrams_model_path(path))
   mut m = dict(8192)
   mut total = 0.0
   mut i = 0
   def n = txt.len
   while i < n {
      while i < n && (load8(txt, i) == 10 || load8(txt, i) == 13 || load8(txt, i) == 32 || load8(txt, i) == 9) { i += 1 }
      if i + 4 <= n {
         def c0 = load8(txt, i)
         def c1 = load8(txt, i + 1)
         def c2 = load8(txt, i + 2)
         def c3 = load8(txt, i + 3)
         if c0 >= 65 && c0 <= 90 && c1 >= 65 && c1 <= 90 && c2 >= 65 && c2 <= 90 && c3 >= 65 && c3 <= 90 {
            def gram = str.str_slice(txt, i, i + 4)
            i += 4
            while i < n && (load8(txt, i) == 32 || load8(txt, i) == 9) { i += 1 }
            mut cnt_i = 0
            while i < n && load8(txt, i) >= 48 && load8(txt, i) <= 57 {
               cnt_i = cnt_i * 10 + load8(txt, i) - 48
               i += 1
            }
            if cnt_i > 0 {
               m = m.set(gram, float(cnt_i))
               total = total + float(cnt_i)
            }
         }
      }
      while i < n && load8(txt, i) != 10 { i += 1 }
      i += 1
   }
   if total <= 0.0 { panic("quadgrams: empty model") }
   def denom = total
   def floor_p = 0.01 / denom
   def floor_log = log(floor_p)
   def keys = dict_keys(m)
   mut packed = _quad_filled_table(floor_log)
   mut ki = 0
   while ki < keys.len {
      def k, v = keys.get(ki), m.get(k, 0.0)
      def lp = log(v / denom)
      m = m.set(k, lp)
      packed[_quad_pack4(k, 0)] = lp
      ki += 1
   }
   _quad_loaded = true
   _quad_logp = m
   _quad_logp_packed = packed
   _quad_packed_loaded = true
   _quad_floor = floor_log
   [dict_clone(_quad_logp), _quad_floor]
}

fn quadgrams_score(str s) f64 {
   "Scores s using the quadgram model. Higher is better.
   Assumes s already consists of uppercase A-Z letters."
   if !_quad_packed_loaded { _quadgrams_load_packed() }
   def m = _quad_logp_packed
   def floor_log = _quad_floor
   def n = s.len
   if n < 4 { return 0.0 }
   mut score = 0.0
   mut i = 0
   while i + 4 <= n {
      score = score + m[_quad_pack4(s, i)]
      i += 1
   }
   score
}

fn quadgrams_score_upper_ascii(str s) f64 {
   "Convenience: uppercases and strips non A-Z before scoring."
   if !_quad_packed_loaded { _quadgrams_load_packed() }
   def m = _quad_logp_packed
   mut score = 0.0
   mut packed = 0
   mut alpha_count = 0
   mut i = 0
   def n = s.len
   while i < n {
      def v = _quad_alpha_value(load8(s, i))
      if v >= 0 {
         if alpha_count < 3 {
            packed = packed * 26 + v
            alpha_count += 1
         } else {
            packed = (packed % 17576) * 26 + v
            score += m[packed]
            alpha_count += 1
         }
      }
      i += 1
   }
   score
}
