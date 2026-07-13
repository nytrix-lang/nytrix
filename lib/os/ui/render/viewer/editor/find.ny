;; Keywords: editor find search filter linebar os ui render viewer text
;; Find, filter, and linebar search helpers for editor buffers.
;; References:
;; - std.os.ui.render.viewer.editor.core
module std.os.ui.render.viewer.editor.find(
   state, open, close, is_open, query, set_query, error, results, count, index, current,
   replacement, set_replacement, replace_on, show_replace, hide_replace, toggle_replace,
   active_field, toggle_field, toggle_regex, toggle_case, toggle_word, regex_on, case_on, word_on,
   refresh, next, prev, replace_current, replace_all, summary, handle_key, handle_char
)

use std.core
use std.core.str as str
use std.core.regex as regex
use std.math (max, min)
use std.os.ui.window
use std.os.ui.window.consts as key

def MAX_RESULTS = 5000

;; Returns the result of the `state` operation.
fn state() dict {
   {
      "open": false, "query": "", "regex": false, "case": false, "word": false,
      "replace_open": false, "replace": "", "field": "find",
      "results": [], "index": 0, "error": ""
   }
}

;; Returns the result of the `open` operation.
fn open(dict st) dict { st["open"] = true st }

;; Closes resources owned by the state and returns the closed state.
fn close(dict st) dict { st["open"] = false st }

fn is_open(dict st) bool { bool(st.get("open", false)) }

fn query(dict st) str { to_str(st.get("query", "")) }

fn replacement(dict st) str { to_str(st.get("replace", "")) }

fn error(dict st) str { to_str(st.get("error", "")) }

fn results(dict st) list { st.get("results", []) }

;; Returns the result of the `count` operation.
fn count(dict st) int { results(st).len }

;; Returns the result of the `index` operation.
fn index(dict st) int {
   def n = count(st)
   n <= 0 ? 0 : min(max(0, int(st.get("index", 0))), n - 1)
}

;; Returns the result of the `current` operation.
fn current(dict st) dict {
   def rs = results(st)
   rs.len <= 0 ? dict(8) : rs.get(index(st), dict(8))
}

fn regex_on(dict st) bool { bool(st.get("regex", false)) }

fn case_on(dict st) bool { bool(st.get("case", false)) }

fn word_on(dict st) bool { bool(st.get("word", false)) }

fn replace_on(dict st) bool { bool(st.get("replace_open", false)) }

;; Returns the result of the `active_field` operation.
fn active_field(dict st) str { to_str(st.get("field", "find")) == "replace" && replace_on(st) ? "replace" : "find" }

fn _toggle(dict st, str key_name) dict {
   st[key_name] = !bool(st.get(key_name, false))
   st
}

fn toggle_regex(dict st) dict { _toggle(st, "regex") }

fn toggle_case(dict st) dict { _toggle(st, "case") }

fn toggle_word(dict st) dict { _toggle(st, "word") }

;; Returns the result of the `show_replace` operation.
fn show_replace(dict st) dict {
   st["replace_open"] = true
   st["field"] = "replace"
   st
}

;; Returns the result of the `hide_replace` operation.
fn hide_replace(dict st) dict {
   st["replace_open"] = false
   st["field"] = "find"
   st
}

;; Updates the replace and returns the resulting state.
fn toggle_replace(dict st) dict {
   if replace_on(st) { hide_replace(st) } else { show_replace(st) }
}

;; Updates the field and returns the resulting state.
fn toggle_field(dict st) dict {
   if !replace_on(st) { return st }
   st["field"] = active_field(st) == "replace" ? "find" : "replace"
   st
}

;; Updates the query and returns the resulting state.
fn set_query(dict st, str text) dict {
   st["query"] = text
   st["index"] = 0
   st
}

;; Updates the replacement and returns the resulting state.
fn set_replacement(dict st, str text) dict {
   st["replace"] = text
   st
}

fn _word_byte(int c) bool {
   (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95
}

fn _word_boundary(str line, int start, int end) bool {
   def before = start <= 0 ? 0 : load8(line, start - 1)
   def after = end >= line.len ? 0 : load8(line, end)
   (start <= 0 || !_word_byte(before)) && (end >= line.len || !_word_byte(after))
}

fn _find_from(str text, str needle, int pos) int {
   if needle.len <= 0 || pos > text.len { return -1 }
   def at = str.find(str.str_slice(text, pos, text.len), needle)
   at < 0 ? -1 : pos + at
}

fn _add_result(list out, int line, int start, int end, str text) list {
   if out.len >= MAX_RESULTS || end < start { return out }
   out.append({"line": line, "start": start, "end": end, "text": text})
}

fn _literal_line(list out, str needle, str raw_needle, str line, int li, bool case_sensitive, bool whole_word) list {
   def hay = case_sensitive ? line : str.lower(line)
   mut pos = 0
   while pos <= hay.len && out.len < MAX_RESULTS {
      def at = _find_from(hay, needle, pos)
      if at < 0 { break }
      def end = at + needle.len
      if !whole_word || _word_boundary(line, at, end) {
         out = _add_result(out, li, at, end, str.str_slice(line, at, end))
      }
      pos = max(end, at + 1)
   }
   out
}

fn _regex_line(list out, any rx, str line, int li, bool whole_word) list {
   def ms = regex.finditer(rx, line)
   mut i = 0
   while i < ms.len && out.len < MAX_RESULTS {
      def m = ms.get(i)
      def start = regex.start(m, 0)
      def end = regex.end(m, 0)
      if end > start && (!whole_word || _word_boundary(line, start, end)) {
         out = _add_result(out, li, start, end, regex.group(m, 0))
      }
      i += 1
   }
   out
}

fn _nearest_index(list rs, int cursor_line, int cursor_col) int {
   if rs.len <= 0 { return 0 }
   mut i = 0
   while i < rs.len {
      def r = rs.get(i)
      def line = int(r.get("line", 0))
      def start = int(r.get("start", 0))
      if line > cursor_line || (line == cursor_line && start >= cursor_col) { return i }
      i += 1
   }
   0
}

;; Returns the result of the `refresh` operation.
fn refresh(dict st, list lines, int cursor_line=0, int cursor_col=0) dict {
   def q = query(st)
   st["error"] = ""
   if q.len <= 0 {
      st["results"] = []
      st["index"] = 0
      return st
   }
   mut out = []
   if regex_on(st) {
      mut rx = nil
      try {
         rx = regex.compile(q, case_on(st) ? 0 : regex.IGNORECASE)
      } catch e {
         st["results"] = []
         st["index"] = 0
         st["error"] = to_str(e)
         return st
      }
      mut i = 0
      while i < lines.len && out.len < MAX_RESULTS {
         out = _regex_line(out, rx, to_str(lines.get(i, "")), i, word_on(st))
         i += 1
      }
   } else {
      def needle = case_on(st) ? q : str.lower(q)
      mut i = 0
      while i < lines.len && out.len < MAX_RESULTS {
         out = _literal_line(out, needle, q, to_str(lines.get(i, "")), i, case_on(st), word_on(st))
         i += 1
      }
   }
   st["results"] = out
   st["index"] = _nearest_index(out, cursor_line, cursor_col)
   st
}

fn _move(dict st, int dir) dict {
   def n = count(st)
   if n <= 0 { return st }
   st["index"] = (index(st) + dir + n) % n
   st
}

fn next(dict st) dict { _move(st, 1) }

fn prev(dict st) dict { _move(st, -1) }

;; Returns the result of the `summary` operation.
fn summary(dict st) str {
   if error(st).len > 0 { return "bad regex" }
   def n = count(st)
   n <= 0 ? "0/0" : to_str(index(st) + 1) + "/" + to_str(n)
}

fn _drop_last(str s) str {
   s.len <= 0 ? "" : str.str_slice(s, 0, s.len - 1)
}

fn _flags(dict st) int { case_on(st) ? 0 : regex.IGNORECASE }

fn _replace_span(str line, int start, int end, str repl) str {
   str.str_slice(line, 0, start) + repl + str.str_slice(line, end, line.len)
}

fn _replacement_for(dict st, dict r, str line) dict {
   def repl = replacement(st)
   if !regex_on(st) { return {"ok": true, "text": repl, "error": ""} }
   def start = min(max(int(r.get("start", 0)), 0), line.len)
   def end = min(max(int(r.get("end", start)), start), line.len)
   def segment = str.str_slice(line, start, end)
   try {
      return {"ok": true, "text": regex.sub(query(st), repl, segment, 1, _flags(st)), "error": ""}
   } catch e {
      return {"ok": false, "text": "", "error": to_str(e)}
   }
}

fn _replace_result(dict st, list lines, dict r) dict {
   def li = int(r.get("line", -1))
   if li < 0 || li >= lines.len { return {"ok": false, "lines": lines, "error": "match out of range", "line": 0, "col": 0} }
   def line = to_str(lines.get(li, ""))
   def start = min(max(int(r.get("start", 0)), 0), line.len)
   def end = min(max(int(r.get("end", start)), start), line.len)
   def rr = _replacement_for(st, r, line)
   if !bool(rr.get("ok", false)) { return {"ok": false, "lines": lines, "error": to_str(rr.get("error", "replace failed")), "line": li, "col": start} }
   mut out = clone(lines)
   def repl = to_str(rr.get("text", ""))
   out[li] = _replace_span(line, start, end, repl)
   {"ok": true, "lines": out, "error": "", "line": li, "col": start + repl.len}
}

;; Updates the current and returns the resulting state.
fn replace_current(dict st, list lines) dict {
   if count(st) <= 0 { return {"ok": true, "st": st, "lines": lines, "count": 0, "error": "", "line": 0, "col": 0} }
   def r = current(st)
   def rr = _replace_result(st, lines, r)
   st["error"] = to_str(rr.get("error", ""))
   {"ok": bool(rr.get("ok", false)), "st": st, "lines": rr.get("lines", lines), "count": bool(rr.get("ok", false)) ? 1 : 0, "error": to_str(rr.get("error", "")), "line": int(rr.get("line", 0)), "col": int(rr.get("col", 0))}
}

;; Updates the all and returns the resulting state.
fn replace_all(dict st, list lines) dict {
   def rs = results(st)
   if rs.len <= 0 { return {"ok": true, "st": st, "lines": lines, "count": 0, "error": "", "line": 0, "col": 0} }
   mut out = clone(lines)
   mut count = 0
   mut last_line = 0
   mut last_col = 0
   mut i = rs.len - 1
   while i >= 0 {
      def rr = _replace_result(st, out, rs.get(i, dict(0)))
      if !bool(rr.get("ok", false)) {
         st["error"] = to_str(rr.get("error", "replace failed"))
         return {"ok": false, "st": st, "lines": out, "count": count, "error": to_str(rr.get("error", "replace failed")), "line": last_line, "col": last_col}
      }
      out = rr.get("lines", out)
      last_line = int(rr.get("line", last_line))
      last_col = int(rr.get("col", last_col))
      count += 1
      i -= 1
   }
   st["error"] = ""
   {"ok": true, "st": st, "lines": out, "count": count, "error": "", "line": last_line, "col": last_col}
}

;; Handles the key operation and returns the resulting state.
fn handle_key(dict st, any data) dict {
   mut action = ""
   if window.event_key_is(data, key.KEY_ESCAPE) { st = close(st) action = "close" }
   elif window.event_key_is(data, key.KEY_TAB) && replace_on(st) { st = toggle_field(st) action = "field" }
   elif window.event_key_is(data, key.KEY_ENTER) {
      def mods = int(data.get("mods", data.get("mod", 0)))
      if replace_on(st) && (mods & key.MOD_CONTROL) != 0 { action = "replace-all" }
      elif replace_on(st) && active_field(st) == "replace" { action = "replace-current" }
      else {
         st = (mods & key.MOD_SHIFT) != 0 ? prev(st) : next(st)
         action = "jump"
      }
   } elif window.event_key_is(data, key.KEY_BACKSPACE) {
      if active_field(st) == "replace" {
         st = set_replacement(st, _drop_last(replacement(st)))
         action = "replace-text"
      } else {
         st = set_query(st, _drop_last(query(st)))
         action = "refresh"
      }
   }
   {"st": st, "action": action}
}

;; Handles the char operation and returns the resulting state.
fn handle_char(dict st, any data) dict {
   def mods = int(data.get("mods", data.get("mod", 0)))
   if (mods & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0 { return st }
   def cp = int(data.get("char", 0))
   if cp >= 32 && cp != 127 {
      if active_field(st) == "replace" { st = set_replacement(st, replacement(st) + chr(cp)) }
      else { st = set_query(st, query(st) + chr(cp)) }
   }
   st
}

#main {
   def lines = ["one two", "Two tone", "stone"]
   mut st = refresh(set_query(open(state()), "two"), lines, 0, 0)
   assert(results(st).len == 2, "literal case-insensitive find count")
   assert(int(current(st).get("line", -1)) == 0, "literal case-insensitive find cursor")
   st = refresh(toggle_word(st), lines, 0, 0)
   assert(results(st).len == 2, "word boundary keeps words")
   st = refresh(set_query(toggle_regex(st), "tw[a-z]+"), lines, 0, 0)
   assert(results(st).len == 2, "regex find")
   st = set_replacement(st, "XX")
   def one = replace_current(st, lines)
   assert(one.get("ok", false) && one.get("count", 0) == 1 && one.get("lines", []).get(0, "") == "one XX", "replace current")
   st = refresh(set_query(st, "(tw)(o)"), lines, 0, 0)
   st = set_replacement(st, "\\2\\1")
   def all = replace_all(st, lines)
   assert(all.get("ok", false) && all.get("count", 0) == 2 && all.get("lines", []).get(0, "") == "one otw", "regex replace all")
   st = refresh(set_query(st, "("), lines, 0, 0)
   assert(error(st).len > 0, "regex errors are captured")
}
