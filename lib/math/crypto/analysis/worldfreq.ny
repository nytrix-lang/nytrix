;; Keywords: analysis frequency-analysis language-model worldfreq
;; Cryptanalysis scoring and recovery routines for language frequency profiles, n-gram scoring, and detection.
module std.math.crypto.analysis.worldfreq(worldfreq_assets_root, worldfreq_stat_root, worldfreq_profile_names, worldfreq_profile_exists, worldfreq_profile_paths, worldfreq_load, worldfreq_alpha_upper, worldfreq_word_counter, worldfreq_word_rows, worldfreq_rows_to_table, worldfreq_word_table, worldfreq_word_stats, worldfreq_ngram_counter, worldfreq_ngram_rows, worldfreq_ngram_table, worldfreq_ngram_score, worldfreq_score_text, worldfreq_detect)
use std.core
use std.os (file_exists, file_read)
use std.os.fs as osfs
use std.os.path as ospath
use std.core.str as str

mut _worldfreq_profiles = dict(4)
mut _worldfreq_ngram_profiles = dict(4)
mut _worldfreq_names_cache = 0

def _WF_PACK1 = 26
def _WF_PACK2 = 676
def _WF_PACK3 = 17576
def _WF_PACK4 = 456976

@inline
fn _is_ascii_upper(int: c): bool { c >= 65 && c <= 90 }

@inline
fn _ascii_alpha_value(int: c): int {
   if(c >= 65 && c <= 90){ return c - 65 }
   if(c >= 97 && c <= 122){ return c - 97 }
   -1
}

fn _normalize_lang(any: lang): str {
   def clean = is_str(lang) ? str.lower(str.strip(lang)) : ""
   clean.len == 0 ? "english" : clean
}

fn worldfreq_assets_root(): str {
   "Returns the repo-local dictionary asset root."
   ospath.resolve_repo_asset("etc/assets/dict")
}

fn worldfreq_stat_root(): str {
   "Returns the repo-local statistical language profile root."
   worldfreq_assets_root()
}

fn _alpha_token_ok(str: token, int: min_len=1, int: max_len=24): bool {
   def n = token.len
   if(n < min_len || n > max_len){ return false }
   mut i = 0
   while(i < n){
      if(!_is_ascii_upper(load8(token, i))){ return false }
      i += 1
   }
   true
}

fn _weight_bucket(int: count, int: bias=0): int {
   if(count <= 0){ return 0 }
   mut bits = 0
   mut v = count
   while(v > 1){
      v = v / 2
      bits += 1
   }
   (bits > bias) ? (bits - bias) : 0
}

fn _packed_size_for_len(int: n): int {
   case n {
      1 -> _WF_PACK1
      2 -> _WF_PACK2
      3 -> _WF_PACK3
      4 -> _WF_PACK4
      _ -> 0
   }
}

fn _zero_int_table(int: n): list {
   mut xs = list(n)
   mut i = 0
   while(i < n){
      xs[i] = 0
      i += 1
   }
   store64(xs, n, 0)
   xs
}

fn _load_weighted_alpha_table_pair(str: path, int: min_len=1, int: max_len=24, int: exact_len=0, int: bias=0): list {
   def pack_size = _packed_size_for_len(exact_len)
   mut packed = pack_size > 0 ? _zero_int_table(pack_size) : []
   if(path.len == 0 || !file_exists(path)){ return [dict(0), packed] }
   match file_read(path){
      ok(txt) -> {
         mut out = dict(1024)
         def n = txt.len
         mut i = 0
         while(i < n){
            while(i < n && (load8(txt, i) == 10 || load8(txt, i) == 13 || load8(txt, i) == 32 || load8(txt, i) == 9)){ i += 1 }
            def start = i
            mut pack = 0
            mut token_len = 0
            mut alpha_ok = true
            while(i < n){
               def v = _ascii_alpha_value(load8(txt, i))
               if(v < 0){ break }
               if(token_len < 4){ pack = pack * 26 + v }
               token_len += 1
               i += 1
            }
            if(token_len == 0){ alpha_ok = false }
            while(i < n && (load8(txt, i) == 32 || load8(txt, i) == 9)){ i += 1 }
            mut count = 0
            while(i < n && load8(txt, i) >= 48 && load8(txt, i) <= 57){
               count = count * 10 + load8(txt, i) - 48
               i += 1
            }
            def expect_ok = (exact_len > 0) ? (token_len == exact_len) : (token_len >= min_len && token_len <= max_len)
            if(alpha_ok && expect_ok && count > 0){
               def weight = _weight_bucket(count, bias)
               if(weight > 0){
                  def token = str.str_slice(txt, start, start + token_len)
                  out = out.set(token, weight)
                  if(pack_size > 0){ packed[pack] = weight }
               }
            }
            while(i < n && load8(txt, i) != 10){ i += 1 }
            i += 1
         }
         [out, packed]
      }
      err(ignorederr) -> {
         ignorederr
         def empty = dict(0)
         [empty, packed]
      }
   }
}

fn _load_weighted_alpha_table(str: path, int: min_len=1, int: max_len=24, int: exact_len=0, int: bias=0): dict {
   _load_weighted_alpha_table_pair(path, min_len, max_len, exact_len, bias)[0]
}

fn _load_weighted_alpha_packed(str: path, int: exact_len, int: bias=0): list {
   def pack_size = _packed_size_for_len(exact_len)
   mut packed = _zero_int_table(pack_size)
   if(path.len == 0 || !file_exists(path)){ return packed }
   match file_read(path){
      ok(txt) -> {
         def n = txt.len
         mut i = 0
         while(i < n){
            while(i < n && (load8(txt, i) == 10 || load8(txt, i) == 13 || load8(txt, i) == 32 || load8(txt, i) == 9)){ i += 1 }
            mut pack = 0
            mut token_len = 0
            while(i < n){
               def v = _ascii_alpha_value(load8(txt, i))
               if(v < 0){ break }
               pack = pack * 26 + v
               token_len += 1
               i += 1
            }
            while(i < n && (load8(txt, i) == 32 || load8(txt, i) == 9)){ i += 1 }
            mut count = 0
            while(i < n && load8(txt, i) >= 48 && load8(txt, i) <= 57){
               count = count * 10 + load8(txt, i) - 48
               i += 1
            }
            if(token_len == exact_len && count > 0){
               def weight = _weight_bucket(count, bias)
               if(weight > 0){ packed[pack] = weight }
            }
            while(i < n && load8(txt, i) != 10){ i += 1 }
            i += 1
         }
         packed
      }
      err(ignorederr) -> { ignorederr packed }
   }
}

fn _worldfreq_load_ngram_cached(any: lang="english"): dict {
   def name = _normalize_lang(lang)
   if(_worldfreq_ngram_profiles.contains(name)){ return _worldfreq_ngram_profiles.get(name, dict(0)) }
   def paths = worldfreq_profile_paths(name)
   def profile = {
      "lang": name,
      "monograms_packed": _load_weighted_alpha_packed(paths.get("monograms_path", ""), 1, 20),
      "bigrams_packed": _load_weighted_alpha_packed(paths.get("bigrams_path", ""), 2, 10),
      "trigrams_packed": _load_weighted_alpha_packed(paths.get("trigrams_path", ""), 3, 12),
      "quadgrams_packed": _load_weighted_alpha_packed(paths.get("quadgrams_path", ""), 4, 12)
   }
   _worldfreq_ngram_profiles = _worldfreq_ngram_profiles.set(name, profile)
   profile
}

fn _load_alpha_word_set(str: path, int: min_len=2, int: max_len=24): dict {
   if(path.len == 0 || !file_exists(path)){ return dict(0) }
   match file_read(path){
      ok(txt) -> {
         mut out = dict(1024)
         def n = txt.len
         mut line_start = 0
         mut i = 0
         while(i <= n){
            if(i == n || load8(txt, i) == 10){
               def token = upper(str.strip(str.str_slice(txt, line_start, i)))
               line_start = i + 1
               if(_alpha_token_ok(token, min_len, max_len)){ out = out.set(token, true) }
            }
            i += 1
         }
         out
      }
      err(ignorederr) -> { ignorederr dict(0) }
   }
}

fn _merge_word_set(dict: base, dict: extra): dict {
   mut out = base
   def keys = dict_keys(extra)
   mut i = 0
   while(i < keys.len){
      out = out.set(keys[i], true)
      i += 1
   }
   out
}

fn _profile_name_from_filename(str: name): str {
   if(!str.endswith(name, ".txt")){ return "" }
   def dash = str.find(name, "-")
   if(dash <= 0){ return "" }
   def suffix = str.str_slice(name, dash + 1, name.len)
   case suffix {
      "words.txt", "monograms.txt", "bigrams.txt", "trigrams.txt", "quadgrams.txt" -> str.str_slice(name, 0, dash)
      _ -> ""
   }
}

fn _sort_strings(list: xs): list {
   def out = clone(xs)
   mut i = 1
   while(i < out.len){
      def item = out[i]
      mut j = i - 1
      while(j >= 0 && out[j] > item){
         out[j + 1] = out[j]
         j -= 1
      }
      out[j + 1] = item
      i += 1
   }
   out
}

fn _sort_rows_desc(list: rows): list {
   def out = clone(rows)
   mut i = 1
   while(i < out.len){
      def row = out[i]
      def row_score = row[1]
      mut j = i - 1
      while(j >= 0 && out[j][1] < row_score){
         out[j + 1] = out[j]
         j -= 1
      }
      out[j + 1] = row
      i += 1
   }
   out
}

fn _rows_limit(list: rows, int: limit): list {
   if(limit <= 0 || rows.len <= limit){ return rows }
   mut out = []
   mut i = 0
   while(i < rows.len && i < limit){
      out = out.append(rows[i])
      i += 1
   }
   out
}

fn worldfreq_profile_names(): list {
   "Lists discovered language profiles from `etc/assets/dict`."
   if(is_list(_worldfreq_names_cache)){ return clone(_worldfreq_names_cache) }
   def root = worldfreq_stat_root()
   if(!osfs.is_dir(root)){ return [] }
   def entries = osfs.list_dir(root)
   mut seen = dict(8)
   mut names = []
   mut i = 0
   while(i < entries.len){
      def lang = _profile_name_from_filename(entries[i])
      if(lang.len > 0 && !seen.contains(lang)){
         seen = seen.set(lang, true)
         names = names.append(lang)
      }
      i += 1
   }
   def sorted = _sort_strings(names)
   _worldfreq_names_cache = sorted
   clone(sorted)
}

fn worldfreq_profile_paths(any: lang="english"): dict {
   "Returns the expected asset paths for language profile `lang`."
   def name = _normalize_lang(lang)
   def base = worldfreq_assets_root()
   def stat = worldfreq_stat_root()
   mut raw_words = ""
   if(name == "english"){ raw_words = ospath.join(base, "words.txt") } else {
      def alt = ospath.join(base, name + "-words.txt")
      if(file_exists(alt)){ raw_words = alt }
   }
   {
      "lang": name,
      "root": base,
      "stat_root": stat,
      "raw_words_path": raw_words,
      "common_words_path": ospath.join(stat, name + "-words.txt"),
      "monograms_path": ospath.join(stat, name + "-monograms.txt"),
      "bigrams_path": ospath.join(stat, name + "-bigrams.txt"),
      "trigrams_path": ospath.join(stat, name + "-trigrams.txt"),
      "quadgrams_path": ospath.join(stat, name + "-quadgrams.txt")
   }
}

fn worldfreq_profile_exists(any: lang="english"): bool {
   "Returns true when the weighted language profile files for `lang` are present."
   def paths = worldfreq_profile_paths(lang)
   file_exists(paths.get("common_words_path", "")) &&
   file_exists(paths.get("monograms_path", "")) &&
   file_exists(paths.get("bigrams_path", "")) &&
   file_exists(paths.get("trigrams_path", "")) &&
   file_exists(paths.get("quadgrams_path", ""))
}

fn _worldfreq_load_cached(any: lang="english"): dict {
   def name = _normalize_lang(lang)
   if(_worldfreq_profiles.contains(name)){ return _worldfreq_profiles.get(name, dict(0)) }
   def paths = worldfreq_profile_paths(name)
   def common_words = _load_weighted_alpha_table(paths.get("common_words_path", ""), 1, 24, 0, 12)
   def mono_pair = _load_weighted_alpha_table_pair(paths.get("monograms_path", ""), 1, 1, 1, 20)
   def bi_pair = _load_weighted_alpha_table_pair(paths.get("bigrams_path", ""), 2, 2, 2, 10)
   def tri_pair = _load_weighted_alpha_table_pair(paths.get("trigrams_path", ""), 3, 3, 3, 12)
   def quad_pair = _load_weighted_alpha_table_pair(paths.get("quadgrams_path", ""), 4, 4, 4, 12)
   mut word_set = _load_alpha_word_set(paths.get("raw_words_path", ""), 2, 24)
   word_set = _merge_word_set(word_set, common_words)
   def profile = {
      "lang": name,
      "paths": paths,
      "word_set": word_set,
      "common_words": common_words,
      "monograms": mono_pair[0],
      "bigrams": bi_pair[0],
      "trigrams": tri_pair[0],
      "quadgrams": quad_pair[0],
      "monograms_packed": mono_pair[1],
      "bigrams_packed": bi_pair[1],
      "trigrams_packed": tri_pair[1],
      "quadgrams_packed": quad_pair[1]
   }
   _worldfreq_profiles = _worldfreq_profiles.set(name, profile)
   profile
}

fn worldfreq_load(any: lang="english"): dict {
   "Loads and caches a weighted language profile. The return value is a dict with tables and asset paths."
   dict_clone(_worldfreq_load_cached(lang))
}

fn _profile_resolve(any: profile_or_lang): dict {
   if(is_dict(profile_or_lang)){ return profile_or_lang }
   _worldfreq_load_cached(profile_or_lang)
}

fn worldfreq_alpha_upper(str: text): str {
   "Uppercases and strips non-ASCII letters from `text`."
   mut out = str.Builder(max(16, text.len + 8))
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      if(c >= 97 && c <= 122){ out = str.builder_append(out, str.chr(c - 32)) } elif(_is_ascii_upper(c)){ out = str.builder_append(out, str.chr(c)) }
      i += 1
   }
   def s = str.builder_to_str(out)
   str.builder_free(out)
   s
}

fn worldfreq_word_counter(str: text, int: min_len=1, int: max_len=24): dict {
   "Counts uppercase ASCII word tokens found in `text`."
   def upper_text = upper(text)
   mut out = dict(16)
   def n = upper_text.len
   mut i = 0
   while(i < n){
      while(i < n && !_is_ascii_upper(load8(upper_text, i))){ i += 1 }
      def start = i
      while(i < n && _is_ascii_upper(load8(upper_text, i))){ i += 1 }
      if(i > start){
         def token = str.str_slice(upper_text, start, i)
         if(_alpha_token_ok(token, min_len, max_len)){ out = out.set(token, int(out.get(token, 0)) + 1) }
      }
   }
   out
}

fn worldfreq_word_rows(str: text, int: limit=32, int: min_len=1, int: max_len=24): list {
   "Returns `[word, count]` rows for `text`, sorted by descending count."
   def rows = _sort_rows_desc(items(worldfreq_word_counter(text, min_len, max_len)))
   _rows_limit(rows, limit)
}

fn worldfreq_rows_to_table(list: rows): str {
   "Formats `[token, count]` rows into `TOKEN COUNT` lines."
   mut out = str.Builder(max(32, rows.len * 12))
   mut i = 0
   while(i < rows.len){
      def row = rows[i]
      out = str.builder_append(out, to_str(row[0]))
      out = str.builder_append(out, " ")
      out = str.builder_append(out, to_str(row[1]))
      out = str.builder_append(out, "\n")
      i += 1
   }
   def s = str.builder_to_str(out)
   str.builder_free(out)
   s
}

fn worldfreq_word_table(str: text, int: limit=0, int: min_len=1, int: max_len=24): str {
   "Builds a `TOKEN COUNT` table for word frequencies in `text`."
   worldfreq_rows_to_table(worldfreq_word_rows(text, limit, min_len, max_len))
}

fn worldfreq_word_stats(str: text, any: profile_or_lang="english"): list {
   "Returns `[score, token_count, dict_hits, common_hits]` for `text` under the selected profile."
   def profile = _profile_resolve(profile_or_lang)
   def common_words = profile.get("common_words", dict(0))
   def word_set = profile.get("word_set", dict(0))
   def upper_text = upper(text)
   mut score = 0
   mut token_count = 0
   mut dict_hits = 0
   mut common_hits = 0
   def n = upper_text.len
   mut i = 0
   while(i < n){
      while(i < n && !_is_ascii_upper(load8(upper_text, i))){ i += 1 }
      def start = i
      while(i < n && _is_ascii_upper(load8(upper_text, i))){ i += 1 }
      if(i > start){
         def token = str.str_slice(upper_text, start, i)
         token_count += 1
         def common_weight = int(common_words.get(token, 0))
         if(common_weight > 0){
            common_hits += 1
            dict_hits += 1
            score += common_weight * 24 + min(12, token.len)
         } elif(word_set.contains(token)){
            dict_hits += 1
            score += 18 + min(10, token.len)
         } elif(token.len >= 8){
            score -= 14
         } elif(token.len >= 5){
            score -= 6
         }
      }
   }
   [score, token_count, dict_hits, common_hits]
}

fn worldfreq_ngram_counter(str: text, int: n, bool: alpha_only=true): dict {
   "Counts `n`-grams from `text`. By default only A-Z characters are used."
   if(n <= 0){ return dict(0) }
   def src = alpha_only ? worldfreq_alpha_upper(text) : upper(text)
   if(src.len < n){ return dict(0) }
   mut out = dict(16)
   mut i = 0
   while(i + n <= src.len){
      def gram = str.str_slice(src, i, i + n)
      out = out.set(gram, int(out.get(gram, 0)) + 1)
      i += 1
   }
   out
}

fn worldfreq_ngram_rows(str: text, int: n, int: limit=32, bool: alpha_only=true): list {
   "Returns `[ngram, count]` rows for `text`, sorted by descending count."
   def rows = _sort_rows_desc(items(worldfreq_ngram_counter(text, n, alpha_only)))
   _rows_limit(rows, limit)
}

fn worldfreq_ngram_table(str: text, int: n, int: limit=0, bool: alpha_only=true): str {
   "Builds a `TOKEN COUNT` table for `n`-gram frequencies in `text`."
   worldfreq_rows_to_table(worldfreq_ngram_rows(text, n, limit, alpha_only))
}

fn worldfreq_ngram_score(str: text, any: profile_or_lang="english"): int {
   "Scores `text` against the profile's monogram through quadgram tables."
   def profile = is_dict(profile_or_lang) ? profile_or_lang : _worldfreq_load_ngram_cached(profile_or_lang)
   def monograms = profile.get("monograms_packed", [])
   def bigrams = profile.get("bigrams_packed", [])
   def trigrams = profile.get("trigrams_packed", [])
   def quadgrams = profile.get("quadgrams_packed", [])
   if(monograms.len < _WF_PACK1 || bigrams.len < _WF_PACK2 || trigrams.len < _WF_PACK3 || quadgrams.len < _WF_PACK4){
      def mono_dict = profile.get("monograms", dict(0))
      def bi_dict = profile.get("bigrams", dict(0))
      def tri_dict = profile.get("trigrams", dict(0))
      def quad_dict = profile.get("quadgrams", dict(0))
      def alpha_text = worldfreq_alpha_upper(text)
      def n = alpha_text.len
      mut slow_score = 0
      mut slow_i = 0
      while(slow_i < n){
         slow_score += int(mono_dict.get(str.str_slice(alpha_text, slow_i, slow_i + 1), 0))
         slow_i += 1
      }
      slow_i = 0
      while(slow_i + 2 <= n){
         slow_score += int(bi_dict.get(str.str_slice(alpha_text, slow_i, slow_i + 2), 0)) * 2 - 1
         slow_i += 1
      }
      slow_i = 0
      while(slow_i + 3 <= n){
         def tri = int(tri_dict.get(str.str_slice(alpha_text, slow_i, slow_i + 3), 0))
         slow_score += (tri > 0) ? (tri * 4) : -2
         slow_i += 1
      }
      slow_i = 0
      while(slow_i + 4 <= n){
         def quad = int(quad_dict.get(str.str_slice(alpha_text, slow_i, slow_i + 4), 0))
         slow_score += (quad > 0) ? (quad * 6) : -3
         slow_i += 1
      }
      return slow_score
   }
   mut score = 0
   mut i = 0
   mut count = 0
   mut p1 = 0
   mut p2 = 0
   mut p3 = 0
   while(i < text.len){
      def v = _ascii_alpha_value(load8(text, i))
      if(v >= 0){
         score += int(monograms[v])
         if(count >= 1){
            def b = p1 * 26 + v
            score += int(bigrams[b]) * 2 - 1
            if(count >= 2){
               def t = p2 * 26 + v
               def tw = int(trigrams[t])
               score += (tw > 0) ? (tw * 4) : -2
               if(count >= 3){
                  def q = p3 * 26 + v
                  def qw = int(quadgrams[q])
                  score += (qw > 0) ? (qw * 6) : -3
               }
               p3 = t
            }
            p2 = b
         }
         p1 = v
         count += 1
      }
      i += 1
   }
   score
}

fn worldfreq_score_text(str: text, any: profile_or_lang="english"): int {
   "Returns the combined word-model and n-gram score for `text`."
   def profile = _profile_resolve(profile_or_lang)
   def word_stats = worldfreq_word_stats(text, profile)
   int(word_stats[0]) + worldfreq_ngram_score(text, profile)
}

fn worldfreq_detect(str: text, any: langs=nil, int: limit=8): list {
   "Scores `text` against discovered language profiles and returns `[lang, score, dict_hits, common_hits]` rows."
   def profile_names = is_list(langs) ? langs : worldfreq_profile_names()
   mut rows = []
   mut i = 0
   while(i < profile_names.len){
      def name = profile_names[i]
      if(worldfreq_profile_exists(name)){
         def profile = worldfreq_load(name)
         def word_stats = worldfreq_word_stats(text, profile)
         def score = int(word_stats[0]) + worldfreq_ngram_score(text, profile)
         rows = rows.append([
               profile.get("lang", name),
               score,
               int(word_stats[2]),
               int(word_stats[3])
         ])
      }
      i += 1
   }
   _rows_limit(_sort_rows_desc(rows), limit)
}
