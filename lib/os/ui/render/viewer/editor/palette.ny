;; Keywords: editor palette command search actions os ui render viewer text
;; Command palette filtering, ranking, and action dispatch helpers.
;; References:
;; - std.os.ui.render.viewer.editor.commands
module std.os.ui.render.viewer.editor.palette(
   commands, matches, which_key,
   state, open, close, is_open, query, index, set_index,
   scroll, visible, set_visible, scroll_by, visible_matches,
   state_matches, selected, handle_key, handle_char
)

use std.core
use std.core.str as str
use std.os.ui.render.viewer.editor.commands as cmd
use std.os.ui.window
use std.os.ui.window.consts as key

fn commands() list {
   cmd.commands()
}

fn state() dict {
   {"open": false, "query": "", "index": 0, "scroll": 0, "visible": 12, "cfg": cmd.config()}
}

fn open(dict st) dict {
   st["open"] = true
   st["query"] = ""
   st["index"] = 0
   st["scroll"] = 0
   st
}

fn close(dict st) dict {
   st["open"] = false
   st
}

fn is_open(dict st) bool { bool(st.get("open", false)) }

fn query(dict st) str { to_str(st.get("query", "")) }

fn index(dict st) int { int(st.get("index", 0)) }

fn scroll(dict st) int { int(st.get("scroll", 0)) }

fn visible(dict st) int { max(1, int(st.get("visible", 12))) }

fn _cfg(dict st) dict { st.get("cfg", cmd.config()) }

fn _blob(any row) str {
   def title = str.lower(to_str(row.get(0, "")))
   def id = str.lower(to_str(row.get(1, "")))
   def key = str.lower(to_str(row.get(2, "")))
   def tag = str.lower(to_str(row.get(4, "")))
   title + " " + id + " " + key + " " + tag
}

fn _fuzzy(str q, str s) bool {
   if q.len <= 0 { return true }
   mut qi = 0
   mut si = 0
   while qi < q.len && si < s.len {
      if load8(q, qi) == load8(s, si) { qi += 1 }
      si += 1
   }
   qi == q.len
}

fn _match_level(str q, any row) int {
   if q.len <= 0 { return 0 }
   def title = str.lower(to_str(row.get(0, "")))
   def id = str.lower(to_str(row.get(1, "")))
   def key = str.lower(to_str(row.get(2, "")))
   def tag = str.lower(to_str(row.get(4, "")))
   if str.startswith(title, q) || str.startswith(id, q) || str.startswith(key, q) { return 0 }
   if str.str_contains(title, q) || str.str_contains(id, q) || str.str_contains(key, q) || str.str_contains(tag, q) { return 1 }
   _fuzzy(q, _blob(row)) ? 2 : -1
}

fn _append_level(list out, list rows, str q, int level, int limit) list {
   mut i = 0
   while i < rows.len && out.len < limit {
      def row = rows.get(i)
      if _match_level(q, row) == level { out = out.append(row) }
      i += 1
   }
   out
}

fn matches(str query, int limit=1000000) list {
   def q = str.lower(str.strip(query))
   def rows = cmd.enabled(cmd.config())
   mut out = []
   out = _append_level(out, rows, q, 0, limit)
   out = _append_level(out, rows, q, 1, limit)
   out = _append_level(out, rows, q, 2, limit)
   out
}

fn state_matches(dict st, int limit=1000000) list {
   def q = str.lower(str.strip(query(st)))
   def rows = cmd.enabled(_cfg(st))
   mut out = []
   out = _append_level(out, rows, q, 0, limit)
   out = _append_level(out, rows, q, 1, limit)
   out = _append_level(out, rows, q, 2, limit)
   out
}

fn _clamp_scroll(int pos, int total, int view) int {
   min(max(0, pos), max(0, total - max(1, view)))
}

fn _clamp_state(dict st, int total) dict {
   def view = visible(st)
   def idx = min(max(0, index(st)), max(0, total - 1))
   mut scr = _clamp_scroll(scroll(st), total, view)
   if idx < scr { scr = idx }
   if idx >= scr + view { scr = idx - view + 1 }
   st["index"] = idx
   st["scroll"] = _clamp_scroll(scr, total, view)
   st
}

fn set_visible(dict st, int rows) dict {
   st["visible"] = max(1, rows)
   _clamp_state(st, state_matches(st).len)
}

fn set_index(dict st, int idx) dict {
   def opts = state_matches(st)
   st["index"] = min(max(0, idx), max(0, opts.len - 1))
   _clamp_state(st, opts.len)
}

fn scroll_by(dict st, int delta) dict {
   def opts = state_matches(st)
   def view = visible(st)
   st["scroll"] = _clamp_scroll(scroll(st) + delta, opts.len, view)
   def idx = index(st)
   if idx < scroll(st) { st["index"] = scroll(st) }
   elif idx >= scroll(st) + view { st["index"] = min(max(0, opts.len - 1), scroll(st) + view - 1) }
   _clamp_state(st, opts.len)
}

fn visible_matches(dict st) list {
   def opts = state_matches(st)
   def start = _clamp_scroll(scroll(st), opts.len, visible(st))
   def stop = min(opts.len, start + visible(st))
   mut out = []
   mut i = start
   while i < stop {
      out = out.append(opts.get(i))
      i += 1
   }
   out
}

fn selected(dict st) list {
   def opts = state_matches(st)
   opts.len <= 0 ? [] : opts.get(min(max(index(st), 0), opts.len - 1))
}

fn handle_key(dict st, any data) dict {
   mut choose = false
   def opts = state_matches(st)
   def mods = int(data.get("mods", data.get("mod", 0)))
   def ctrl = (mods & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0
   if window.event_key_is(data, key.KEY_ESCAPE) { st = close(st) }
   elif window.event_key_is(data, key.KEY_ENTER) { choose = true }
   elif window.event_key_is(data, key.KEY_BACKSPACE) {
      def q = query(st)
      if q.len > 0 { st["query"] = str.str_slice(q, 0, q.len - 1) }
      st["index"] = 0
      st["scroll"] = 0
   } else {
      if window.event_key_is(data, key.KEY_UP) || (ctrl && window.event_key_is(data, key.KEY_P)) { st = set_index(st, index(st) - 1) }
      elif window.event_key_is(data, key.KEY_DOWN) || window.event_key_is(data, key.KEY_TAB) || (ctrl && window.event_key_is(data, key.KEY_N)) { st = set_index(st, index(st) + 1) }
      elif window.event_key_is(data, key.KEY_PAGE_UP) { st = set_index(st, index(st) - visible(st)) }
      elif window.event_key_is(data, key.KEY_PAGE_DOWN) { st = set_index(st, index(st) + visible(st)) }
      elif window.event_key_is(data, key.KEY_HOME) { st = set_index(st, 0) }
      elif window.event_key_is(data, key.KEY_END) { st = set_index(st, opts.len - 1) }
   }
   {"st": st, "choose": choose}
}

fn handle_char(dict st, any data) dict {
   def mods = int(data.get("mods", data.get("mod", 0)))
   if (mods & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0 { return st }
   def cp = int(data.get("char", 0))
   if cp >= 32 && cp != 127 {
      st["query"] = query(st) + chr(cp)
      st["index"] = 0
      st["scroll"] = 0
   }
   st
}

fn which_key(str prefix) list {
   cmd.which_key(prefix)
}

#main {
   mut st = open(state())
   st = handle_char(st, {"char": 114})
   st = set_visible(st, 3)
   st = scroll_by(st, 2)
   assert(commands().len > 0 && selected(st).len > 0 && which_key("C-SPC").len > 0 && visible_matches(st).len <= 3, "editor palette")
   print("✓ viewer editor palette test passed")
}
