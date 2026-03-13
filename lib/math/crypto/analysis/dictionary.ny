;; Keywords: analysis dictionary wordlist cracking
;; Dictionary and wordlist attack routines for hashes, tokens, and classical-cipher candidates.
;; References:
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap12.pdf
module std.math.crypto.analysis.dictionary(dictionary_attack, dictionary_attack_stream, dictionary_attack_stream_str, dictionary_attack_stream_predicate, dictionary_attack_salt, dictionary_attack_default, default_wordlist_paths, rockyou_wordlist_path, analysis_wordlist_path, load_wordlist, load_wordlist_limit, load_analysis_words_limit, analysis_word_exists, analysis_select_words, analysis_phrase_word, analysis_segment_text, analysis_dictionary_attack_predicate, leet_rules_apply, leet_variants_full, case_variants_ascii, case_variants_ascii_full, dictionary_candidate_variants, dictionary_candidate_variants_full_case, dictionary_crack_many, analysis_dictionary_crack_many, decimal_suffix, candidate_suffixes, dictionary_crack_suffixes, dictionary_crack_suffix_digits, analysis_dictionary_crack_suffix_digits, dictionary_crack_many_with_variants, analysis_dictionary_crack_many_with_variants, unix_crypt_available, unix_crypt_hash, analysis_unix_crypt_crack_many, analysis_unix_crypt_crack_many_exact)
use std.core
use std.core.mem
use std.math.bin
use std.math.crypto.hash
use std.math.crypto.analysis.worldfreq
use std.os (file_exists, file_read)
use std.os.path as ospath
use std.os.sys
use std.core.str as str

if(comptime{ __os_name() == "linux" }){
   #link "libcrypt.so.1"
   #include <crypt.h> as ""
}

mut _wordlist_cache = dict(8)
mut _analysis_score_cache = dict(4)
mut _analysis_segment_cache = dict(8)

fn dictionary_attack(str: target_hash, str: wordlist_fn, fnptr: hash_fn): dict {
   "Try each word from a wordlist against a target hash.
   target_hash: the hash to crack as hex string
   wordlist_fn: filename of wordlist(each line is one candidate password)
   hash_fn: function(data_bytes) -> hex string of hash
   Returns a dict with:
   .found: boolean indicating if password was found
   .password: the recovered password(empty if not found)
   .attempts: number of words tried"
   def wordlist = _load_wordlist(wordlist_fn)
   def res = _dictionary_attack_scan(wordlist, target_hash, hash_fn, nil, 0)
   _dict_result(res[0], res[1], res[2])
}

fn _dict_result(bool: found, str: password, int: attempts): dict {
   {"found": found, "password": password, "attempts": attempts}
}

fn _dict_word_match(str: word, str: target_hash, fnptr: hash_fn, any: salt, int: mode=0): bool {
   def wb = word.to_bytes
   if(mode == 1){ return hash_fn(salt, wb) == target_hash }
   hash_fn(wb) == target_hash
}

fn _dictionary_attack_scan(list: wordlist, str: target_hash, fnptr: hash_fn, any: salt=nil, int: mode=0): list {
   mut attempts = 0
   mut found = false
   mut password = ""
   mut wi = 0
   while(wi < wordlist.len && !found){
      def word = wordlist[wi]
      attempts += 1
      if(_dict_word_match(word, target_hash, hash_fn, salt, mode)){
         found = true
         password = word
         wi = wordlist.len
      }
      wi += 1
   }
   [found, password, attempts]
}

fn _dict_line_match(str: line, str: target_hash, fnptr: hash_fn, fnptr: pred_fn, int: mode): bool {
   if(mode == 2){ return pred_fn(line) }
   if(mode == 1){ return hash_fn(line) == target_hash }
   hash_fn(line.to_bytes) == target_hash
}

fn _noop_hash(any: _): str { "" }

fn _noop_pred(any: _): bool { false }

fn _stream_line_clean(str: line): str {
   mut out = line
   if(str.endswith(out, "\r")){ out = str.str_slice(out, 0, out.len - 1) }
   str.strip(out)
}

fn _dictionary_stream_core(str: target_hash, str: wordlist_fn, fnptr: hash_fn, fnptr: pred_fn, int: max_words=0, int: mode=0): dict {
   if(!is_str(wordlist_fn) || wordlist_fn.len == 0){ return dict(3) }
   if(!file_exists(wordlist_fn)){ return dict(3) }
   if(str.endswith(wordlist_fn, ".gz")){ return dict(3) }
   match sys_open(wordlist_fn, 0, 0){
      err(ignorederr) -> { ignorederr _dict_result(false, "", 0) }
      ok(fd) -> {
         mut attempts = 0
         mut found = false
         mut password = ""
         mut carry = ""
         def max_carry = 1 << 20
         def chunk_n = 1 << 16
         mut buf = malloc(chunk_n + 32)
         while(!found){
            def got = sys_read(fd, buf, chunk_n)
            mut nread = 0
            match got {
               ok(r) -> { nread = r }
               err(ignorederr) -> { ignorederr  nread = -1 }
            }
            if(nread <= 0){ break }
            def chunk = init_str(buf, nread)
            mut s = chunk
            if(carry.len > 0){
               if(carry.len > max_carry){ carry = "" }
               s = str_add(carry, chunk)
               carry = ""
            }
            mut start = 0
            mut i = 0
            def n = s.len
            while(i <= n && !found){
               if(i == n || load8(s, i) == 10){
                  if(i == n){ carry = str.str_slice(s, start, i) } else {
                     def line = _stream_line_clean(str.str_slice(s, start, i))
                     if(line.len > 0){
                        attempts += 1
                        if(_dict_line_match(line, target_hash, hash_fn, pred_fn, mode)){
                           found = true
                           password = line
                        } elif(max_words > 0 && attempts >= max_words){
                           found = false
                           i = n + 1
                           break
                        }
                     }
                     start = i + 1
                  }
               }
               i += 1
            }
            if(max_words > 0 && attempts >= max_words){ break }
         }
         unwrap(sys_close(fd))
         free(buf)
         if(!found && carry.len > 0 && (max_words <= 0 || attempts < max_words)){
            def line2 = _stream_line_clean(carry)
            if(line2.len > 0){
               attempts += 1
               if(_dict_line_match(line2, target_hash, hash_fn, pred_fn, mode)){
                  found = true
                  password = line2
               }
            }
         }
         _dict_result(found, password, attempts)
      }
   }
}

fn dictionary_attack_stream(str: target_hash, str: wordlist_fn, fnptr: hash_fn, int: max_words=0): dict {
   "Streaming variant of dictionary_attack(does not load whole wordlist into memory).
   max_words: 0 means no limit."
   _dictionary_stream_core(target_hash, wordlist_fn, hash_fn, _noop_pred, max_words, 0)
}

fn dictionary_attack_stream_str(str: target_hash, str: wordlist_fn, fnptr: hash_fn, int: max_words=0): dict {
   "Streaming dictionary attack where hash_fn takes the candidate as a string.
   This avoids allocating a byte-list for each word and is significantly faster
   for large wordlists."
   _dictionary_stream_core(target_hash, wordlist_fn, hash_fn, _noop_pred, max_words, 1)
}

fn dictionary_attack_stream_predicate(str: wordlist_fn, fnptr: pred_fn, int: max_words=0): dict {
   "Streaming dictionary scan where pred_fn(candidate_string) -> bool.
   Returns dict with found/password/attempts."
   _dictionary_stream_core("", wordlist_fn, _noop_hash, pred_fn, max_words, 2)
}

fn analysis_dictionary_attack_predicate(fnptr: pred_fn, int: max_words=0, str: lang="english", int: min_len=1, int: max_len=24): dict {
   "Scan repo-local analysis dictionary words with pred_fn(candidate) -> bool."
   def path = analysis_wordlist_path(lang)
   def words = load_analysis_words_limit(max_words, lang, min_len, max_len)
   mut attempts = 0
   mut i = 0
   while(i < words.len){
      def word = words[i]
      attempts += 1
      if(pred_fn(word)){ return _dict_result(true, word, attempts).merge({"wordlist": path}) }
      i += 1
   }
   _dict_result(false, "", attempts).merge({"wordlist": path})
}

fn dictionary_attack_salt(str: target_hash, any: salt, str: wordlist_fn, fnptr: hash_fn): dict {
   "Try salt+word combinations against a target hash.
   target_hash: the hash to crack as hex string
   salt: salt value as byte list
   wordlist_fn: filename of wordlist
   hash_fn: function(salt_bytes, word_bytes) -> hex string of hash
   Returns a dict with:
   .found: boolean indicating if password was found
   .password: the recovered password(empty if not found)
   .attempts: number of words tried"
   def wordlist = _load_wordlist(wordlist_fn)
   def res = _dictionary_attack_scan(wordlist, target_hash, hash_fn, salt, 1)
   _dict_result(res[0], res[1], res[2])
}

fn dictionary_attack_default(str: target_hash, fnptr: hash_fn): dict {
   "Try the default local system wordlist.
   RockYou is preferred when present at common Kali/Linux paths."
   def path = rockyou_wordlist_path()
   if(path.len == 0){
      mut result = _dict_result(false, "", 0)
      result = result.set("wordlist", "")
      return result
   }
   mut r = dictionary_attack(target_hash, path, hash_fn)
   r = r.set("wordlist", path)
   r
}

fn default_wordlist_paths(): list {
   "Candidate system wordlists, ordered by preference.
   The first two cover the common Kali and system-dict RockYou locations."
   [
      "/usr/share/dict/rockyou.txt",
      "/usr/share/wordlists/rockyou.txt",
      "/usr/share/dict/rockyou.txt.gz",
      "/usr/share/wordlists/rockyou.txt.gz",
      "/usr/share/dict/words"
   ]
}

fn rockyou_wordlist_path(): str {
   "Return the first existing RockYou/system wordlist path, or empty string."
   def paths = default_wordlist_paths()
   mut i = 0
   while(i < paths.len){
      def p = paths[i]
      if(file_exists(p)){ return p }
      i += 1
   }
   ""
}

fn analysis_wordlist_path(str: lang="english"): str {
   "Return the repo-local crypto analysis dictionary path for `lang`, or empty string."
   def paths = worldfreq.worldfreq_profile_paths(lang)
   def p = paths.get("common_words_path", "")
   if(is_str(p) && p.len > 0 && file_exists(p)){ return p }
   ""
}

fn load_wordlist(str: filename): list {
   "Load a newline-delimited wordlist into a list of stripped non-empty strings."
   _load_wordlist(filename)
}

fn load_wordlist_limit(str: filename, int: max_words): list {
   "Load at most max_words stripped non-empty lines from a plaintext wordlist."
   if(max_words <= 0){ return list(0) }
   if(!is_str(filename) || filename.len == 0){ return list(0) }
   if(!file_exists(filename)){ return list(0) }
   if(str.endswith(filename, ".gz")){ return list(0) }
   match file_read(filename){
      ok(content) -> {
         mut out = list(0)
         mut start = 0
         mut i = 0
         def n = content.len
         while(i <= n && out.len < max_words){
            if(i == n || load8(content, i) == 10){
               mut line = str.str_slice(content, start, i)
               if(str.endswith(line, "\r")){ line = str.str_slice(line, 0, line.len - 1) }
               line = str.strip(line)
               if(line.len > 0){ out = out.append(line) }
               start = i + 1
            }
            i += 1
         }
         out
      }
      err(ignorederr) -> { ignorederr list(0) }
   }
}

fn _ascii_is_alpha(int: c): bool {
   case c {
      65..90, 97..122 -> true
      _ -> false
   }
}

fn _ascii_lower_byte(int: c): int {
   case c {
      65..90 -> c + 32
      _ -> c
   }
}

fn _ascii_upper_byte(int: c): int {
   case c {
      97..122 -> c - 32
      _ -> c
   }
}

fn _ascii_lower_chr(int: c): str {
   str.chr(_ascii_lower_byte(c))
}

fn _analysis_word_from_line(str: text, int: start, int: stop, int: min_len, int: max_len): str {
   mut i, n = start, 0
   mut b = Builder(max(8, stop - start + 1))
   while(i < stop){
      def c = load8(text, i)
      if(c == 32 || c == 9 || c == 13){ break }
      if(!_ascii_is_alpha(c)){
         builder_free(b)
         return ""
      }
      b = builder_append(b, _ascii_lower_chr(c))
      n += 1
      i += 1
   }
   if(n < min_len || n > max_len){
      builder_free(b)
      return ""
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn load_analysis_words_limit(int: max_words=0, str: lang="english", int: min_len=1, int: max_len=24): list {
   "Load lowercase words from the repo-local analysis dictionary.
   The source is `std.math.crypto.analysis.worldfreq`'s weighted word profile, so
   this is reproducible from the repository and does not depend on system
   wordlists. `max_words <= 0` means no limit."
   def path = analysis_wordlist_path(lang)
   if(path.len == 0){ return list(0) }
   match file_read(path){
      ok(content) -> {
         mut out = list(0)
         mut start = 0
         mut i = 0
         def n = content.len
         while(i <= n){
            if(i == n || load8(content, i) == 10){
               def w = _analysis_word_from_line(content, start, i, min_len, max_len)
               if(w.len > 0){ out = out.append(w) }
               if(max_words > 0 && out.len >= max_words){ return out }
               start = i + 1
            }
            i += 1
         }
         out
      }
      err(ignorederr) -> { ignorederr list(0) }
   }
}

fn analysis_word_exists(str: word, str: lang="english", int: min_len=1, int: max_len=24): bool {
   "Return whether `word` exists in the repo-local analysis dictionary."
   analysis_select_words([word], lang, min_len, max_len).len == 1
}

fn _analysis_norm_token(any: token): str {
   if(!is_str(token)){ return "" }
   str.lower(str.strip(token))
}

fn analysis_select_words(list: tokens, str: lang="english", int: min_len=1, int: max_len=24): list {
   "Return the requested tokens that are present in the repo-local analysis dictionary."
   mut wanted = list(0)
   mut wanted_set = dict(tokens.len * 2 + 1)
   mut i = 0
   while(i < tokens.len){
      def w = _analysis_norm_token(tokens[i])
      if(w.len >= min_len && w.len <= max_len && !wanted_set.contains(w)){
         wanted = wanted.append(w)
         wanted_set = wanted_set.set(w, true)
      }
      i += 1
   }
   if(wanted.len == 0){ return list(0) }
   def path = analysis_wordlist_path(lang)
   if(path.len == 0){ return list(0) }
   mut found_count = 0
   mut found_set = dict(wanted.len * 2 + 1)
   match file_read(path){
      ok(content) -> {
         mut start = 0
         mut pos = 0
         def n = content.len
         while(pos <= n && found_count < wanted.len){
            if(pos == n || load8(content, pos) == 10){
               def w = _analysis_word_from_line(content, start, pos, min_len, max_len)
               if(wanted_set.contains(w) && !found_set.contains(w)){
                  found_set = found_set.set(w, true)
                  found_count += 1
               }
               start = pos + 1
            }
            pos += 1
         }
      }
      err(ignorederr) -> { ignorederr }
   }
   mut out = list(0)
   i = 0
   while(i < tokens.len){
      def w = _analysis_norm_token(tokens[i])
      if(found_set.contains(w)){ out = out.append(w) }
      i += 1
   }
   out
}

fn analysis_phrase_word(list: tokens, str: sep="", str: lang="english", int: min_len=1, int: max_len=24): str {
   "Join dictionary-validated tokens into a compact phrase candidate.
   Returns an empty string if any token is missing from the analysis dictionary."
   def selected = analysis_select_words(tokens, lang, min_len, max_len)
   if(selected.len != tokens.len){ return "" }
   mut out = ""
   mut i = 0
   while(i < selected.len){
      if(i > 0){ out = out + sep }
      out = out + selected[i]
      i += 1
   }
   out
}

fn _load_analysis_word_scores_limit(int: max_words=20000, str: lang="english", int: min_len=1, int: max_len=24): dict {
   def cache_key = lang + ":" + to_str(max_words) + ":" + to_str(min_len) + ":" + to_str(max_len)
   if(_analysis_score_cache.contains(cache_key)){ return _analysis_score_cache.get(cache_key, dict(0)) }
   def words = load_analysis_words_limit(max_words, lang, min_len, max_len)
   mut scores = dict(words.len * 2 + 1)
   mut i = 0
   while(i < words.len){
      def w = words[i]
      if(_analysis_segment_word_ok(w)){ scores = scores.set(w, (words.len - i) * 5 + w.len * w.len * 2000 - 70000) }
      i += 1
   }
   _analysis_score_cache = _analysis_score_cache.set(cache_key, scores)
   scores
}

fn _analysis_segment_word_ok(str: w): bool {
   if(w.len > 2){ return true }
   if(w == "a" || w == "i"){ return true }
   def common_two = [
      "of", "to", "in", "is", "it", "as", "at", "he", "be", "by",
      "on", "or", "if", "me", "we", "us", "up", "so", "no", "my",
      "an", "am", "do", "go"
   ]
   common_two.contains(w)
}

fn _alpha_compact_lower(str: text): str {
   mut b, i = Builder(max(8, text.len + 1)), 0
   while(i < text.len){
      def c = load8(text, i)
      if(_ascii_is_alpha(c)){ b = builder_append_byte(b, _ascii_lower_byte(c)) }
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _analysis_segment_cache_key(str: text, int: max_words, str: lang, int: min_len, int: max_len): str {
   to_str(max_words) + ":" + lang + ":" + to_str(min_len) + ":" + to_str(max_len) + ":" + text
}

fn analysis_segment_text(str: text, int: max_words=20000, str: lang="english", int: min_len=1, int: max_len=24): str {
   "Segment compact alphabetic text into likely dictionary words using repo-local word frequencies."
   def cache_key = _analysis_segment_cache_key(text, max_words, lang, min_len, max_len)
   if(_analysis_segment_cache.contains(cache_key)){ return _analysis_segment_cache.get(cache_key, "") }
   def s, n = _alpha_compact_lower(text), s.len
   if(n == 0){
      _analysis_segment_cache = _analysis_segment_cache.set(cache_key, "")
      return ""
   }
   def scores = _load_analysis_word_scores_limit(max_words, lang, min_len, max_len)
   if(scores.len == 0){
      _analysis_segment_cache = _analysis_segment_cache.set(cache_key, s)
      return s
   }
   def neg = -1000000000
   mut best = list(0)
   mut prev = list(0)
   mut word_at = list(0)
   mut i = 0
   while(i <= n){
      best = best.append(neg)
      prev = prev.append(-1)
      word_at = word_at.append("")
      i += 1
   }
   best[0] = 0
   mut pos = 0
   while(pos < n){
      if(best[pos] > neg){
         mut stop = pos + min_len
         while(stop <= n && stop - pos <= max_len){
            def w = str.str_slice(s, pos, stop)
            if(scores.contains(w)){
               def score = scores.get(w, 0)
               def candidate_score = best[pos] + score
               if(candidate_score > best[stop]){
                  best[stop] = candidate_score
                  prev[stop] = pos
                  word_at[stop] = w
               }
            }
            stop += 1
         }
      }
      pos += 1
   }
   if(best[n] <= neg){
      _analysis_segment_cache = _analysis_segment_cache.set(cache_key, s)
      return s
   }
   mut pieces = list(0)
   pos = n
   while(pos > 0){
      def w = word_at[pos]
      if(w.len == 0){
         _analysis_segment_cache = _analysis_segment_cache.set(cache_key, s)
         return s
      }
      pieces = pieces.append(w)
      pos = prev[pos]
   }
   mut out_b = Builder(max(16, s.len + pieces.len))
   i = pieces.len
   while(i > 0){
      i -= 1
      if(i < pieces.len - 1){ out_b = builder_append_byte(out_b, 32) }
      out_b = builder_append(out_b, pieces[i])
   }
   def out = builder_to_str(out_b)
   builder_free(out_b)
   _analysis_segment_cache = _analysis_segment_cache.set(cache_key, out)
   out
}

fn leet_rules_apply(str: word): list {
   "Generate leet-speak variants of a word.
   Applies common substitutions: e->3, a->4, o->0, s->5, t->7, i->1, l->1.
   Returns a list of variant strings."
   leet_variants_full(word, 4096)
}

fn _leet_options_full(int: c): list {
   def ch = str.chr(c)
   case c {
      97 -> [ch, "4", "@"]
      98 -> [ch, "6"]
      101 -> [ch, "3"]
      105 -> [ch, "1"]
      108 -> [ch, "1"]
      111 -> [ch, "0"]
      115 -> [ch, "5", "$"]
      116 -> [ch, "7"]
      _ -> [ch]
   }
}

fn leet_variants_full(str: word, int: max_variants=4096): list {
   "Generate full leetspeak combinations for common audit substitutions.
   Includes a->4/@, e->3, i->1, o->0, s->5/$, t->7."
   def lower_word = str.lower(word)
   mut out = [""]
   mut pos = 0
   while(pos < lower_word.len){
      def opts = _leet_options_full(load8(lower_word, pos))
      if(out.len * opts.len > max_variants){ return _dedup_strings(out) }
      out = _variant_extend(out, opts)
      pos += 1
   }
   _dedup_strings(out)
}

fn _capitalized_ascii(str: word): str {
   if(word.len == 0){ return word }
   mut b, i = Builder(word.len + 8), 0
   while(i < word.len){
      mut c = load8(word, i)
      if(i == 0){ c = _ascii_upper_byte(c) }
      b = builder_append(b, str.chr(c))
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn case_variants_ascii(str: word): list {
   "Return unique ASCII case variants: original, lower, upper, capitalized."
   _dedup_strings([word, str.lower(word), str.upper(word), _capitalized_ascii(word)])
}

fn case_variants_ascii_full(str: word, int: max_variants=4096): list {
   "Generate all ASCII letter case combinations for short candidate words."
   mut out = [""]
   mut pos = 0
   while(pos < word.len){
      def c = load8(word, pos)
      def opts = case c {
         65..90 -> [str.chr(c + 32), str.chr(c)]
         97..122 -> [str.chr(c), str.chr(c - 32)]
         _ -> [str.chr(c)]
      }
      if(out.len * opts.len > max_variants){ return _dedup_strings(out) }
      out = _variant_extend(out, opts)
      pos += 1
   }
   _dedup_strings(out)
}

fn dictionary_candidate_variants(str: word, bool: leet=true, bool: cases=true, int: max_variants=4096): list {
   "Generate dictionary password candidates from one base word.
   Starts with a real dictionary word, optionally expands full leet
   combinations, then applies simple ASCII case rules."
   mut seeds = [word]
   if(leet){ seeds = leet_variants_full(word, max_variants) }
   mut out = list(0)
   mut i = 0
   while(i < seeds.len){
      def seed = seeds[i]
      def vars = cases ? case_variants_ascii(seed) : [seed]
      mut j = 0
      while(j < vars.len){
         out = out.append(vars[j])
         j += 1
      }
      i += 1
   }
   _dedup_strings(out)
}

fn dictionary_candidate_variants_full_case(str: word, bool: leet=true, int: max_variants=4096): list {
   "Generate dictionary candidates with leet transforms and full ASCII case combinations."
   mut seeds = [word]
   if(leet){ seeds = leet_variants_full(word, max_variants) }
   mut out = list(0)
   mut i = 0
   while(i < seeds.len && out.len < max_variants){
      def vars = case_variants_ascii_full(seeds[i], max_variants)
      mut j = 0
      while(j < vars.len && out.len < max_variants){
         out = out.append(vars[j])
         j += 1
      }
      i += 1
   }
   _dedup_strings(out)
}

fn _crack_many_done(dict: found, list: targets): bool {
   mut i = 0
   while(i < targets.len){
      if(!found.contains(targets[i])){ return false }
      i += 1
   }
   true
}

fn _crack_many_result(list: targets, dict: found, int: attempts, int: word_attempts, str: wordlist_path=""): dict {
   mut passwords = list(0)
   mut found_count = 0
   mut i = 0
   while(i < targets.len){
      def pw = found.get(targets[i], "")
      passwords = passwords.append(pw)
      if(pw.len > 0){ found_count += 1 }
      i += 1
   }
   {"found": found_count == targets.len, "found_count": found_count, "passwords": passwords, "by_hash": found,
   "attempts": attempts, "word_attempts": word_attempts, "wordlist": wordlist_path}
}

fn dictionary_crack_many(list: targets, list: words, fnptr: verify_fn, str: wordlist_path=""): dict {
   "Crack several targets using exact dictionary words.
   `verify_fn(candidate, target) -> bool` decides whether a candidate matches a
   target. Returns a dict with `passwords` in target order, `by_hash`, and
   attempt counts."
   mut found = dict(targets.len * 2 + 1)
   mut attempts = 0
   mut wi = 0
   while(wi < words.len && !_crack_many_done(found, targets)){
      def candidate = words[wi]
      mut ti = 0
      while(ti < targets.len){
         def target = targets[ti]
         if(!found.contains(target)){
            attempts += 1
            if(verify_fn(candidate, target)){ found = found.set(target, candidate) }
         }
         ti += 1
      }
      wi += 1
   }
   _crack_many_result(targets, found, attempts, wi, wordlist_path)
}

fn analysis_dictionary_crack_many(list: targets, fnptr: verify_fn, int: max_words=0, str: lang="english", int: min_len=1, int: max_len=24): dict {
   "Crack targets using exact words from the repo-local analysis dictionary."
   def path = analysis_wordlist_path(lang)
   def words = load_analysis_words_limit(max_words, lang, min_len, max_len)
   dictionary_crack_many(targets, words, verify_fn, path)
}

fn _pow10(int: digits): int {
   mut out = 1
   mut i = 0
   while(i < digits){
      out *= 10
      i += 1
   }
   out
}

fn decimal_suffix(int: n, int: width): str {
   "Return `n` as zero-padded decimal text of exactly `width` digits."
   if(width <= 0){ return "" }
   mut div = _pow10(width - 1)
   mut b = Builder(width + 1)
   while(div > 0){
      def digit = (n / div) % 10
      b = builder_append(b, str.chr(48 + digit))
      div = div / 10
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _suffix_symbols(any: symbols): list {
   if(is_list(symbols) && symbols.len > 0){ return symbols }
   [""]
}

fn candidate_suffixes(list: symbols=[""], int: min_digits=0, int: max_digits=0, bool: symbol_before_digits=true, bool: digits_before_symbol=true): list {
   "Generate reusable suffix strings for dictionary cracking.
   For each digit width this emits decimal text, plus optional symbol+digits and
   digits+symbol forms. Empty symbols produce only the digit text."
   mut out = list(0)
   def suffix_symbols = _suffix_symbols(symbols)
   mut width = max(0, min_digits)
   while(width <= max_digits){
      def limit = _pow10(width)
      mut n = 0
      while(n < limit){
         def digits = decimal_suffix(n, width)
         mut si = 0
         while(si < suffix_symbols.len){
            def sym = suffix_symbols[si]
            if(sym.len == 0){
               out = out.append(digits)
            } else {
               if(symbol_before_digits){ out = out.append(sym + digits) }
               if(digits_before_symbol){ out = out.append(digits + sym) }
            }
            si += 1
         }
         n += 1
      }
      width += 1
   }
   _dedup_strings(out)
}

fn dictionary_crack_suffixes(list: targets, list: words, list: suffixes, fnptr: verify_fn, str: wordlist_path=""): dict {
   "Crack targets with candidates formed as `word + suffix`."
   mut found = dict(targets.len * 2 + 1)
   mut attempts = 0
   mut word_attempts = 0
   mut wi = 0
   while(wi < words.len && !_crack_many_done(found, targets)){
      def base = words[wi]
      word_attempts += 1
      mut si = 0
      while(si < suffixes.len && !_crack_many_done(found, targets)){
         def candidate = base + suffixes[si]
         mut ti = 0
         while(ti < targets.len){
            def target = targets[ti]
            if(!found.contains(target)){
               attempts += 1
               if(verify_fn(candidate, target)){ found = found.set(target, candidate) }
            }
            ti += 1
         }
         si += 1
      }
      wi += 1
   }
   _crack_many_result(targets, found, attempts, word_attempts, wordlist_path)
}

fn dictionary_crack_suffix_digits(list: targets, list: words, fnptr: verify_fn, int: min_digits=1, int: max_digits=2, list: symbols=[""], str: wordlist_path=""): dict {
   "Crack targets with candidates `word + decimal_digits + symbol`.
   Useful for common audit/hashcat-style rules such as `baseNN` or `baseNN!`."
   mut found = dict(targets.len * 2 + 1)
   mut attempts = 0
   mut word_attempts = 0
   def suffix_symbols = _suffix_symbols(symbols)
   mut wi = 0
   while(wi < words.len && !_crack_many_done(found, targets)){
      def base = words[wi]
      word_attempts += 1
      mut width = max(0, min_digits)
      while(width <= max_digits && !_crack_many_done(found, targets)){
         def limit = _pow10(width)
         mut n = 0
         while(n < limit && !_crack_many_done(found, targets)){
            def numbered = base + decimal_suffix(n, width)
            mut si = 0
            while(si < suffix_symbols.len && !_crack_many_done(found, targets)){
               def candidate = numbered + suffix_symbols[si]
               mut ti = 0
               while(ti < targets.len){
                  def target = targets[ti]
                  if(!found.contains(target)){
                     attempts += 1
                     if(verify_fn(candidate, target)){ found = found.set(target, candidate) }
                  }
                  ti += 1
               }
               si += 1
            }
            n += 1
         }
         width += 1
      }
      wi += 1
   }
   _crack_many_result(targets, found, attempts, word_attempts, wordlist_path)
}

fn analysis_dictionary_crack_suffix_digits(list: targets, fnptr: verify_fn, int: max_words=0, str: lang="english", int: min_len=1, int: max_len=24, int: min_digits=1, int: max_digits=2, list: symbols=[""]): dict {
   "Crack targets using repo-local dictionary words plus decimal suffix rules."
   def path = analysis_wordlist_path(lang)
   def words = load_analysis_words_limit(max_words, lang, min_len, max_len)
   dictionary_crack_suffix_digits(targets, words, verify_fn, min_digits, max_digits, symbols, path)
}

fn dictionary_crack_many_with_variants(list: targets, list: words, fnptr: verify_fn, int: max_variants=4096): dict {
   "Crack several targets using base words plus generated variants.
   `verify_fn(candidate, target) -> bool` decides whether a candidate matches a
   target. Returns a dict with `passwords` in target order, `by_hash`, and
   attempt counts."
   mut found = dict(targets.len * 2 + 1)
   mut attempts = 0
   mut wi = 0
   while(wi < words.len && !_crack_many_done(found, targets)){
      def base = words[wi]
      def candidates = dictionary_candidate_variants(base, true, true, max_variants)
      mut ci = 0
      while(ci < candidates.len && !_crack_many_done(found, targets)){
         def candidate = candidates[ci]
         mut ti = 0
         while(ti < targets.len){
            def target = targets[ti]
            if(!found.contains(target)){
               attempts += 1
               if(verify_fn(candidate, target)){ found = found.set(target, candidate) }
            }
            ti += 1
         }
         ci += 1
      }
      wi += 1
   }
   _crack_many_result(targets, found, attempts, wi)
}

fn analysis_dictionary_crack_many_with_variants(list: targets, fnptr: verify_fn, int: max_words=0, str: lang="english", int: min_len=1, int: max_len=24, int: max_variants=4096): dict {
   "Crack targets using the repo-local analysis dictionary plus generated variants."
   def words = load_analysis_words_limit(max_words, lang, min_len, max_len)
   dictionary_crack_many_with_variants(targets, words, verify_fn, max_variants).merge({
         "wordlist": analysis_wordlist_path(lang),
   })
}

fn unix_crypt_available(): bool {
   "Return true when libc `crypt(3)` is available through Ny FFI."
   #linux { return true } #else { return false } #endif
}

fn unix_crypt_hash(str: word, str: salt_or_hash): str {
   "Hash `word` with libc `crypt(3)` using `salt_or_hash` as the salt string.
   Passing a full target hash is valid for classic Unix crypt formats."
   #linux {
      def out = crypt(cstr(word), cstr(salt_or_hash))
      return out ? str.cstr_to_str(out) : ""
   } #else {
      return ""
   } #endif
}

fn _unix_crypt_verify(str: candidate, str: target): bool { unix_crypt_hash(candidate, target) == target }

fn analysis_unix_crypt_crack_many(list: targets, int: max_words=0, str: lang="english", int: min_len=1, int: max_len=8, int: max_variants=4096): dict {
   "Crack Unix `crypt(3)` hashes from the repo-local analysis dictionary."
   if(!unix_crypt_available()){ return _crack_many_result(targets, dict(1), 0, 0, analysis_wordlist_path(lang)) }
   analysis_dictionary_crack_many_with_variants(
      targets, _unix_crypt_verify, max_words, lang, min_len, max_len, max_variants
   )
}

fn analysis_unix_crypt_crack_many_exact(list: targets, int: max_words=0, str: lang="english", int: min_len=1, int: max_len=24): dict {
   "Crack Unix `crypt(3)` hashes with exact repo-local analysis dictionary words."
   if(!unix_crypt_available()){ return _crack_many_result(targets, dict(1), 0, 0, analysis_wordlist_path(lang)) }
   analysis_dictionary_crack_many(targets, _unix_crypt_verify, max_words, lang, min_len, max_len)
}

fn _variant_extend(list: prefixes, list: opts): list {
   mut next = list(0)
   mut i = 0
   while(i < prefixes.len){
      mut j = 0
      while(j < opts.len){
         next = next.append(prefixes[i] + opts[j])
         j += 1
      }
      i += 1
   }
   next
}

fn _dedup_strings(list: strings): list {
   def n = strings.len
   if(n == 0){ return list(0) }
   mut result = list(0)
   mut seen = dict(n * 2 + 1)
   mut i = 0
   while(i < n){
      def s = strings[i]
      if(!seen.contains(s)){
         seen = seen.set(s, true)
         result = result.append(s)
      }
      i += 1
   }
   result
}

fn _load_wordlist(str: filename): list {
   if(!is_str(filename) || filename.len == 0){ return list(0) }
   if(!file_exists(filename)){ return list(0) }
   if(str.endswith(filename, ".gz")){
      return list(0)
   }
   if(_wordlist_cache.contains(filename)){ return _wordlist_cache.get(filename, list(0)) }
   match file_read(filename){
      ok(content) -> {
         def lines = str.split(str.str_replace(content, "\r\n", "\n"), "\n")
         mut out = list(0)
         mut i = 0
         while(i < lines.len){
            def w = str.strip(lines[i])
            if(w.len > 0){ out = out.append(w) }
            i += 1
         }
         _wordlist_cache = _wordlist_cache.set(filename, out)
         out
      }
      err(ignorederr) -> { ignorederr list(0) }
   }
}
