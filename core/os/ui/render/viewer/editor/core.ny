;; Keywords: editor core buffer state text os ui render viewer
;; Core editor buffer, cursor, viewport, and state helpers.
;; References:
;; - core.os.ui.render.viewer.editor.selection
module core.os.ui.render.viewer.editor.core(
   split_lines, join_lines, buffer, state,
   current_buffer, current_lines, set_lines, clamp_cursor,
   char_prev, char_next, char_snap
)

use core
use core.str as str
use core.math (max, min)
use core.os.path as ospath

fn split_lines(str text) list {
   "Splits text into editor rows after normalizing CRLF."
   if str.find(text, "\r") < 0 { return str.split(text, "\n") }
   str.split(str.str_replace(str.str_replace(text, "\r\n", "\n"), "\r", "\n"), "\n")
}

fn join_lines(list lines) str {
   "Joins editor rows into file text."
   str.join(lines, "\n")
}

fn buffer(str name, str path, str text) dict {
   "Creates an editor buffer."
   {"name": name.len > 0 ? name : (path.len > 0 ? ospath.basename(path) : "buffer"), "path": path, "lines": split_lines(text), "dirty": false}
}

fn state(list buffers) dict {
   "Creates editor state from a buffer list."
   {
      "buffers": buffers.len > 0 ? buffers : [buffer("scratch", "", "")],
      "active": 0,
      "cursor_line": 0,
      "cursor_col": 0,
      "sel_active": false,
      "sel_a_line": 0,
      "sel_a_col": 0,
      "sel_b_line": 0,
      "sel_b_col": 0,
      "scroll": 0,
      "rail_w": 250.0,
      "drag_divider": false,
      "drag_select": false,
      "status": "ready",
      "status_timer": 0.0,
      "quit_prompt": false,
   }
}

;; Returns the result of the `current_buffer` operation.
fn current_buffer(dict st) dict {
   def bs = st.get("buffers", [])
   def idx = int(st.get("active", 0))
   if idx >= 0 && idx < bs.len {
      def b = bs.get(idx, dict(0))
      if is_dict(b) { return b }
   }
   {"name": "empty", "path": "", "lines": [""], "dirty": false}
}

;; Returns the result of the `current_lines` operation.
fn current_lines(dict st) list {
   def lines = current_buffer(st).get("lines", nil)
   is_list(lines) && lines.len > 0 ? lines : [""]
}

;; Updates the lines and returns the resulting state.
fn set_lines(dict st, list lines) dict {
   mut bs = st.get("buffers", [])
   def idx = int(st.get("active", 0))
   if idx < 0 || idx >= bs.len { return st }
   mut b = bs.get(idx, current_buffer(st))
   b["lines"] = lines.len > 0 ? lines : [""]
   b["dirty"] = true
   bs[idx] = b
   st["buffers"] = bs
   st
}

;; Cursor columns count code points, the unit `str.str_slice` indexes by.
;; Clamping and advancing by byte length (`line.len`) mixed the two units, so
;; any non-ASCII line drifted the caret and could split characters. One step
;; covers a whole visible character: combining marks, variation selectors,
;; skin tones, ZWJ emoji sequences and flag pairs stay with their base. The
;; cluster logic works on bytes (`_b*`) and converts at the edges.
@inline
fn _cont_byte(str s, int i) bool { (load8(s, i) & 192) == 128 }

fn _cp_len(str s, int i) int {
   def int b = load8(s, i)
   b < 128 ? 1 : (b >= 240 ? 4 : (b >= 224 ? 3 : (b >= 192 ? 2 : 1)))
}

fn _cp_at(str s, int i) int {
   def int b = load8(s, i)
   def int n = _cp_len(s, i)
   if n == 1 || i + n > s.len { return b }
   mut int cp = n == 2 ? (b & 31) : (n == 3 ? (b & 15) : (b & 7))
   mut int k = 1
   while k < n {
      cp = (cp << 6) | (load8(s, i + k) & 63)
      k += 1
   }
   cp
}

fn _cp_extends(int cp) bool {
   (cp >= 768 && cp <= 879) || (cp >= 6832 && cp <= 6911) ||
   (cp >= 7616 && cp <= 7679) || (cp >= 8400 && cp <= 8447) ||
   (cp >= 65024 && cp <= 65039) || (cp >= 65056 && cp <= 65071) ||
   cp == 8205 || (cp >= 127995 && cp <= 127999) ||
   (cp >= 917536 && cp <= 917631)
}

@inline
fn _cp_regional(int cp) bool { cp >= 127462 && cp <= 127487 }

fn _bsnap(str line, int col) int {
   if col <= 0 { return 0 }
   if col >= line.len { return line.len }
   mut int c = col
   while c > 0 && _cont_byte(line, c) { c -= 1 }
   c
}

fn _bnext(str line, int col) int {
   def int n = line.len
   mut int i = _bsnap(line, col)
   if i >= n { return n }
   if load8(line, i) < 128 && (i + 1 >= n || load8(line, i + 1) < 128) {
      return i + 1
   }
   def int first = _cp_at(line, i)
   i += _cp_len(line, i)
   if _cp_regional(first) && i < n && _cp_regional(_cp_at(line, i)) {
      return min(n, i + _cp_len(line, i))
   }
   mut joined = first == 8205
   while i < n {
      def int cp = _cp_at(line, i)
      if !joined && !_cp_extends(cp) { break }
      joined = cp == 8205
      i += _cp_len(line, i)
   }
   min(i, n)
}

fn _bprev(str line, int col) int {
   def int c = _bsnap(line, col)
   if c <= 0 { return 0 }
   if load8(line, c - 1) < 128 && (c == 1 || load8(line, c - 2) < 128) {
      return c - 1
   }
   ;; Walk forward from the last ASCII byte before the cursor: clusters only
   ;; extend forward, so the scan stays local on long lines.
   mut int start = c - 1
   while start > 0 && load8(line, start) >= 128 { start -= 1 }
   mut int at = start
   mut int last = start
   while at < c {
      last = at
      at = _bnext(line, at)
   }
   last
}

fn _byte_of(str line, int cp) int {
   mut int i = 0
   mut int k = 0
   while i < line.len && k < cp {
      i += 1
      while i < line.len && _cont_byte(line, i) { i += 1 }
      k += 1
   }
   i
}

fn _cp_of(str line, int b) int {
   mut int k = 0
   mut int i = 0
   while i < b && i < line.len {
      if !_cont_byte(line, i) { k += 1 }
      i += 1
   }
   k
}

fn char_snap(str line, int col) int {
   "Clamps code-point column `col` into `line`."
   if col <= 0 { return 0 }
   min(col, str.utf8_len(line))
}

fn char_next(str line, int col) int {
   "Column just past the visible character starting at code-point `col`."
   if col < 0 { return 0 }
   _cp_of(line, _bnext(line, _byte_of(line, col)))
}

fn char_prev(str line, int col) int {
   "Column where the visible character ending at code-point `col` starts."
   if col <= 0 { return 0 }
   _cp_of(line, _bprev(line, _byte_of(line, col)))
}

;; Returns the result of the `clamp_cursor` operation.
fn clamp_cursor(dict st) dict {
   def lines = current_lines(st)
   mut int row = int(st.get("cursor_line", 0))
   mut int col = int(st.get("cursor_col", 0))
   if row < 0 { row = 0 }
   if row >= lines.len { row = max(0, lines.len - 1) }
   def line = to_str(lines.get(row, ""))
   if col < 0 { col = 0 }
   def int ncols = str.utf8_len(line)
   if col > ncols { col = ncols }
   ;; Preserve the dynamic dictionary ABI after scalar control-flow joins.
   st["cursor_line"] = __box_i64(row)
   st["cursor_col"] = __box_i64(col)
   st
}
