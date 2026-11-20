;; Keywords: editor palette command search actions os ui render viewer text
;; Command palette filtering, ranking, and action dispatch helpers.
;; References:
;; - core.os.ui.render.viewer.editor.commands
module core.os.ui.render.viewer.editor.palette(
   commands, matches, which_key,
   state, open, open_items, items_mode, dismiss, is_open, query, index, set_index,
   scroll, visible, set_visible, scroll_by, visible_matches,
   state_matches, selected, handle_key, wants_choose, handle_char
)

use core
use core.str as str
use core.os.ui.render.viewer.editor.commands as cmd
use core.os.ui.window
use core.os.ui.window.consts as key

;; Returns the result of the `commands` operation.
fn commands() list {
   cmd.commands()
}

;; Returns the result of the `state` operation.
fn state() any {
   {"open": false, "query": "", "index": 0, "scroll": 0, "visible": 12, "cfg": cmd.config()}
}

;; The palette state is passed and returned as `any`. Declared `dict`, a state
;; that went through this module came back as an untagged handle: stored in
;; the caller's state table it no longer tested as a dict, and the palette
;; stayed closed the second time it was opened.
;; Opens the command palette.
fn open(any st) any {
   st["open"] = true
   st["query"] = ""
   st["index"] = 0
   st["scroll"] = 0
   st["items"] = []
   st["mode"] = ""
   st["choose"] = false
   return st
}

;; Opens the palette over an explicit item list instead of the commands.
;; Rows use the command shape [title, id, key, detail, tag]; `mode` names the
;; list (for example "buffers") so the caller knows how to apply a choice.
fn open_items(any st, list rows, str mode) any {
   st = open(st)
   st["items"] = rows
   st["mode"] = mode
   return st
}

;; Returns the active item-list mode, or "" for the command palette.
fn items_mode(any st) str { to_str(st.get("mode", "")) }

;; Hides the palette and returns the state. The name is deliberately unique:
;; as `close` the call `palette.close(state)` ran the OS file-descriptor
;; close, and as `hide` it collided with the window module's `hide`, which
;; also mis-bound the functions exported after it.
fn dismiss(any st) any {
   st["open"] = false
   return st
}

fn is_open(any st) bool { bool(st.get("open", false)) }

fn query(any st) str { to_str(st.get("query", "")) }

fn index(any st) int { int(st.get("index", 0)) }

fn scroll(any st) int { int(st.get("scroll", 0)) }

fn visible(any st) int { max(1, int(st.get("visible", 12))) }

fn _cfg(any st) dict { st.get("cfg", cmd.config()) }

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

;; Returns the result of the `matches` operation.
fn matches(str query, int limit=1000000) list {
   def q = str.lower(str.strip(query))
   def rows = cmd.enabled(cmd.config())
   mut out = []
   out = _append_level(out, rows, q, 0, limit)
   out = _append_level(out, rows, q, 1, limit)
   out = _append_level(out, rows, q, 2, limit)
   out
}

;; Returns the result of the `state_matches` operation.
fn state_matches(any st, int limit=1000000) list {
   def q = str.lower(str.strip(query(st)))
   ;; Chosen with if/else: a `cond ? list : list` conditional read the item
   ;; list shifted by one row, so RET in the buffer picker ran the wrong row.
   mut any rows = st.get("items", [])
   if items_mode(st).len <= 0 || !is_list(rows) { rows = cmd.enabled(_cfg(st)) }
   mut out = []
   out = _append_level(out, rows, q, 0, limit)
   out = _append_level(out, rows, q, 1, limit)
   out = _append_level(out, rows, q, 2, limit)
   out
}

fn _clamp_scroll(int pos, int total, int view) int {
   min(max(0, pos), max(0, total - max(1, view)))
}

fn _clamp_state(any st, int total) any {
   def view = visible(st)
   def idx = min(max(0, index(st)), max(0, total - 1))
   mut scr = _clamp_scroll(scroll(st), total, view)
   if idx < scr { scr = idx }
   if idx >= scr + view { scr = idx - view + 1 }
   st["index"] = idx
   st["scroll"] = _clamp_scroll(scr, total, view)
   return st
}

;; Updates the visible and returns the resulting state.
fn set_visible(any st, int rows) any {
   st["visible"] = max(1, rows)
   _clamp_state(st, state_matches(st).len)
}

;; Updates the index and returns the resulting state.
fn set_index(any st, int idx) any {
   def opts = state_matches(st)
   st["index"] = min(max(0, idx), max(0, opts.len - 1))
   _clamp_state(st, opts.len)
}

;; Returns the result of the `scroll_by` operation.
fn scroll_by(any st, int delta) any {
   def opts = state_matches(st)
   def view = visible(st)
   st["scroll"] = _clamp_scroll(scroll(st) + delta, opts.len, view)
   def idx = index(st)
   if idx < scroll(st) { st["index"] = scroll(st) }
   elif idx >= scroll(st) + view { st["index"] = min(max(0, opts.len - 1), scroll(st) + view - 1) }
   _clamp_state(st, opts.len)
}

;; Returns the result of the `visible_matches` operation.
fn visible_matches(any st) list {
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

;; Returns the result of the `selected` operation.
fn selected(any st) list {
   def opts = state_matches(st)
   if opts.len <= 0 { return [] }
   mut int at = index(st)
   if at < 0 { at = 0 }
   if at >= opts.len { at = opts.len - 1 }
   opts.get(at)
}

;; Handles a key press and returns the resulting state; wants_choose(state)
;; tells whether Enter asked to apply the selection.
fn handle_key(any st, any data) any {
   mut picked = false
   def opts = state_matches(st)
   def mods = int(data.get("mods", data.get("mod", 0)))
   def ctrl = (mods & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0
   if window.event_key_is(data, key.KEY_ESCAPE) { st = dismiss(st) }
   elif window.event_key_is(data, key.KEY_ENTER) { picked = true }
   elif window.event_key_is(data, key.KEY_BACKSPACE) {
      def q = query(st)
      if q.len > 0 {
         def any shorter = to_str(str.str_slice(q, 0, q.len - 1))
         st["query"] = shorter
      }
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
   ;; The state itself is returned, carrying the outcome in "choose". Nesting
   ;; it in a result dict lost it: the caller read the nested state back as a
   ;; non-dict and RET then ran the first command instead of the chosen row.
   ;; `return` is explicit and the flag is not called `choose`: with the
   ;; window module imported by the caller, a bare trailing `st` after that
   ;; assignment returned the flag instead of the state.
   st["choose"] = picked
   return st
}

;; Returns true when the last handle_key asked to apply the selected row.
fn wants_choose(any st) bool { bool(st.get("choose", false)) }

;; Handles the char operation and returns the resulting state.
fn handle_char(any st, any data) any {
   def mods = int(data.get("mods", data.get("mod", 0)))
   if (mods & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0 { return st }
   def cp = int(data.get("char", 0))
   if cp >= 32 && cp != 127 {
      ;; Built as an `any` value: the typed `str` sum stored straight into the
      ;; state read back as a number, so no typed filter ever matched.
      def any next_query = to_str(st.get("query", "")) + str.chr(cp)
      st["query"] = next_query
      st["index"] = 0
      st["scroll"] = 0
   }
   return st
}

;; Finds the key and returns the matching result.
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
