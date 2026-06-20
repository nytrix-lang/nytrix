;; Keywords: editor core buffer state text os ui render viewer
;; Core editor buffer, cursor, viewport, and state helpers.
;; References:
;; - std.os.ui.render.viewer.editor.selection
module std.os.ui.render.viewer.editor.core(
   split_lines, join_lines, buffer, state,
   current_buffer, current_lines, set_lines, clamp_cursor
)

use std.core
use std.core.str as str
use std.math (max)
use std.os.path as ospath

fn split_lines(str text) list {
   "Splits text into editor rows after normalizing CRLF."
   if str.find(text, "\r") < 0 { return str.split(text, "\n") }
   str.split(str.replace(str.replace(text, "\r\n", "\n"), "\r", "\n"), "\n")
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

fn current_buffer(dict st) dict {
   def bs = st.get("buffers", [])
   def idx = int(st.get("active", 0))
   if idx >= 0 && idx < bs.len {
      def b = bs.get(idx, dict(0))
      if is_dict(b) { return b }
   }
   {"name": "empty", "path": "", "lines": [""], "dirty": false}
}

fn current_lines(dict st) list {
   def lines = current_buffer(st).get("lines", nil)
   is_list(lines) && lines.len > 0 ? lines : [""]
}

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

fn clamp_cursor(dict st) dict {
   def lines = current_lines(st)
   mut row = int(st.get("cursor_line", 0))
   mut col = int(st.get("cursor_col", 0))
   if row < 0 { row = 0 }
   if row >= lines.len { row = max(0, lines.len - 1) }
   def line = to_str(lines.get(row, ""))
   if col < 0 { col = 0 }
   if col > line.len { col = line.len }
   st["cursor_line"] = row
   st["cursor_col"] = col
   st
}
