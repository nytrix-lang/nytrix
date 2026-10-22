;; Keywords: documentation search ranking fuzzy matching
;; Pure Ny implementation of documentation search scoring.
;; All scans are byte-oriented (like the C ranker they mirror); only
;; load8/store8-style byte access is used so multibyte UTF-8 text scores
;; identically instead of going through codepoint-indexed slicing.
module core.compiler.tools.doc.rank(fuzzy_score, score, rank)
use core
use core.str as str

fn _lower_byte(int byte) int {
   def lowered = byte >= 65 && byte <= 90 ? byte + 32 : byte
   lowered
}

;; Local min/max helpers: core min/max tail into conditional
;; expressions, which under dynamic `-e` lowering can return the raw
;; tagged integer encoding and silently double/halve scores.
fn _max_int(int a, int b) int {
   if a > b { return a }
   b
}

fn _min_int(int a, int b) int {
   if a < b { return a }
   b
}

fn _boundary(str value, int index) bool {
   if index <= 0 { return true }
   def byte = load8(value, index - 1)
   byte == 46 || byte == 47 || byte == 45 || byte == 95 ||
   byte == 32 || (byte >= 9 && byte <= 13)
}

fn _byte_slice(str s, int start, int stop) str {
   if start < 0 { start = 0 }
   if stop > s.len { stop = s.len }
   if start >= stop { return "" }
   mut out = ""
   mut i = start
   while i < stop {
      out += chr(load8(s, i))
      i += 1
   }
   out
}

fn _find_folded(str candidate, str query) int {
   def int query_len = query.len
   def int last = candidate.len - query_len
   mut int start = 0
   while start <= last {
      mut int index = 0
      while index < query_len &&
      _lower_byte(load8(candidate, start + index)) ==
      _lower_byte(load8(query, index)) {
         index += 1
      }
      if index == query_len { return start }
      start += 1
   }
   -1
}

fn fuzzy_score(str candidate, str query) int {
   "Scores case-insensitive substring and subsequence matches; zero means no match."
   def int candidate_len = candidate.len
   def int query_len = query.len
   if candidate_len == 0 || query_len == 0 { return 0 }
   def hit = _find_folded(candidate, query)
   if hit >= 0 {
      mut score = 10000
      if hit == 0 { score += 2000 }
      if _boundary(candidate, hit) { score += 700 }
      def gap = candidate_len - query_len
      if gap > 0 && gap < 200 { score += 200 - gap }
      return score
   }
   mut score = 0
   mut streak = 0
   mut last = -2
   mut qi = 0
   while qi < query_len {
      def qchar = _lower_byte(load8(query, qi))
      mut found = false
      mut ci = last + 1
      if ci < 0 { ci = 0 }
      while ci < candidate_len {
         if _lower_byte(load8(candidate, ci)) == qchar {
            found = true
            score += 20
            if _boundary(candidate, ci) { score += 20 }
            if ci == last + 1 {
               streak += 1
               score += 15 + streak * 2
            } else {
               streak = 0
            }
            last = ci
            break
         }
         ci += 1
      }
      if !found { return 0 }
      qi += 1
   }
   score
}

fn _field(dict entry, str key) str {
   def value = entry.get(key, "")
   def text = is_str(value) ? str(value) : ""
   text
}

fn _field_score(str value, str term, int weight) int {
   def result = fuzzy_score(value, term)
   def weighted = result > 0 ? result + weight : 0
   weighted
}

fn _query_terms(str query) list {
   mut terms = []
   mut start = -1
   mut i = 0
   while i <= query.len {
      def at_end = i == query.len
      def byte = at_end ? 0 : load8(query, i)
      def separator = at_end || byte == 32 || byte == 9 ||
      byte == 10 || byte == 13
      if separator {
         if start >= 0 {
            terms = terms.append(_byte_slice(query, start, i))
            start = -1
         }
      } elif start < 0 {
         start = i
      }
      i += 1
   }
   terms
}

fn _score_terms(dict entry, str query) int {
   def full = _field(entry, "full_name")
   def name = _field(entry, "name")
   if query.len > 0 {
      if str.lower(full) == str.lower(query) { return 1000000 }
      if str.lower(name) == str.lower(query) { return 950000 }
      def leaf_at = str.find_last(full, ".")
      if leaf_at >= 0 && str.lower(_byte_slice(full, leaf_at + 1, full.len)) ==
      str.lower(query) { return 925000 }
   }
   def terms_list = _query_terms(query)
   def terms = terms_list.len
   if terms == 0 { return 0 }
   mut total = 0
   mut matched = 0
   mut strong = 0
   for term_value in terms_list {
      def term = str(term_value)
      def full_s = _field_score(full, term, 600)
      def name_s = _field_score(name, term, 550)
      def sig_s = _field_score(_field(entry, "signature"), term, 450)
      def keyword_s = _field_score(_field(entry, "keywords"), term, 700)
      def path_s = _field_score(_field(entry, "path"), term, 300)
      def doc_s = _field_score(_field(entry, "doc"), term, 80)
      def code_s = _field_score(_field(entry, "code"), term, 30)
      def best = _max_int(_max_int(_max_int(full_s, name_s),
            _max_int(sig_s, keyword_s)),
         _max_int(_max_int(path_s, doc_s), code_s))
      if best > 0 {
         matched += 1
         if best >= 10000 { strong += 1 }
         total += best + _min_int(full_s, 500) + _min_int(name_s, 650) +
         _min_int(sig_s, 450) + _min_int(keyword_s, 700) +
         _min_int(doc_s, 550) + _min_int(path_s, 180) +
         _min_int(code_s, 80)
      }
   }
   if matched == 0 { return 0 }
   if (terms >= 4 && matched < terms - 1) || (terms == 3 && matched < 2) {
      return 0
   }
   if terms > 1 && matched < terms {
      if terms <= 2 { return 0 }
      if matched * 2 < terms && strong == 0 { return 0 }
      total -= (terms - matched) * 2400
   }
   total += matched * 1200
   if matched == terms { total += 3000 }
   if _field(entry, "kind") == "function" {
      def qualified = full.len > 0 ? full : name
      def dot = str.find_last(qualified, ".")
      def leaf = dot >= 0 ? _byte_slice(qualified, dot + 1, qualified.len) : name
      if str.startswith(leaf, "_") && str.find(query, "_") < 0 {
         total -= 2500
      } elif !str.startswith(leaf, "_") {
         total += 300
      }
   }
   ;; Bind the clamp through a local: a bare conditional as a tail
   ;; expression can surface the runtime's tagged integer encoding.
   def clamped = total > 0 ? total : 1
   clamped
}

fn _query_has_alt(str query) bool {
   mut i = 0
   while i < query.len {
      if load8(query, i) == 124 { return true }
      if load8(query, i) == 92 && i + 1 < query.len &&
      load8(query, i + 1) == 124 { return true }
      i += 1
   }
   false
}

fn score(dict entry, str query) int {
   "Scores a search-index entry using C ny-doc's current field weights."
   ;; The C ranker only trims the branches of an alternative query; a
   ;; query without '|' reaches term scoring verbatim.
   if !_query_has_alt(query) { return _score_terms(entry, query) }
   mut best = 0
   mut start = 0
   mut i = 0
   while i < query.len {
      if load8(query, i) == 124 ||
      (load8(query, i) == 92 && i + 1 < query.len && load8(query, i + 1) == 124) {
         def part = _byte_slice(query, start, i)
         def candidate = _score_terms(entry, str.strip(part))
         best = _max_int(best, candidate)
         i += load8(query, i) == 92 ? 2 : 1
         start = i
      } else {
         i += 1
      }
   }
   def tail = _score_terms(entry, str.strip(_byte_slice(query, start, query.len)))
   best = _max_int(best, tail)
   best
}

fn rank(list entries, str query) list {
   "Returns source indices in stable descending score order."
   mut result = []
   mut scores = []
   mut index = 0
   while index < entries.len {
      def entry = entries.get(index)
      if is_dict(entry) {
         def value = score(entry, query)
         if value > 0 {
            result = result.append(index)
            scores = scores.append(value)
         }
      }
      index += 1
   }
   mut i = 1
   while i < result.len {
      def item = int(result.get(i))
      def item_entry = entries.get(item)
      def item_score = int(scores.get(i))
      mut at = i
      while at > 0 {
         def prior = int(result.get(at - 1))
         def prior_entry = entries.get(prior)
         def prior_score = int(scores.get(at - 1))
         def before = item_score > prior_score ||
         (item_score == prior_score && _field(item_entry, "kind") < _field(prior_entry, "kind")) ||
         (item_score == prior_score && _field(item_entry, "kind") == _field(prior_entry, "kind") &&
            _field(item_entry, "full_name") < _field(prior_entry, "full_name"))
         if !before { break }
         result[at] = prior
         scores[at] = prior_score
         at -= 1
      }
      result[at] = item
      scores[at] = item_score
      i += 1
   }
   result
}

#main {
   def entries = [
      {"kind":"function", "name":"parse", "full_name":"core.math.parse",
         "signature":"parse source", "keywords":"parser syntax", "path":"lib/math/parse.ny"},
      {"kind":"doc", "name":"Compiler", "full_name":"Compiler",
         "keywords":"compiler parser", "path":"docs/compiler.md"}
   ]
   assert(fuzzy_score("core.math.parse", "parse") > 10000,
      "substring scoring rewards leaf matches")
   assert(fuzzy_score("sample", "zzz") == 0,
      "non-matching fuzzy query scores zero")
   assert(score(entries.get(0), "core.math.parse") == 1000000,
      "exact qualified name wins")
   def ordered = rank(entries, "parser")
   assert(ordered.len == 2 && int(ordered.get(0)) == 0,
      "ranking sorts scores and keeps source indices")
   assert(score(entries.get(0), "parse|compiler") ==
      max(score(entries.get(0), "parse"), score(entries.get(0), "compiler")),
      "alternatives choose the best query branch")
   assert(fuzzy_score("résumé parse", "parse") > 10000,
      "byte-oriented scanning matches C on multibyte text")
   print("✓ core.compiler.tools.doc.rank tests passed")
}
