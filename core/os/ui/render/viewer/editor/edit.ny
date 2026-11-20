;; Keywords: editor edit text insert delete indent os ui render viewer
;; Text edit primitives for insertion, deletion, indentation, and line operations.
;; References:
;; - core.os.ui.render.viewer.editor.history
module core.os.ui.render.viewer.editor.edit(
   line_insert, line_delete_before, insert_text, newline, backspace, delete_char,
   switch_buffer, select_buffer
)

use core
use core.str as str
use core.math (max, min)
use core.os.ui.render.viewer.editor.core

@inline
fn _line_at(list lines, int i) str {
   to_str(lines.get(i, ""))
}

;; Line lists are built with `append`, which boxes each element and the result.
;; The earlier preallocated-list construction produced a list whose handle was
;; stored into the buffer dictionary unboxed, so Enter, joining lines, and
;; multi-line paste left the buffer empty.
fn _append_lines(list out, list src, int a, int b) list {
   mut i = a
   while i < b {
      out = out.append(to_str(src.get(i, "")))
      i += 1
   }
   out
}

;; Returns the result of the `line_insert` operation.
fn line_insert(str line, int col, str text) str {
   if text.len <= 0 { return line }
   ;; Clamp into a fresh local: reassigning the `int` parameter from a
   ;; `number`-typed `min`/`max` result halves it (see etc/docs/learn/quirks.md, open representation defects), which
   ;; inserted text at the wrong column.
   def at = int(min(max(col, 0), str.utf8_len(line)))
   str.str_slice(line, 0, at) + text + str.str_slice(line, at, line.len)
}

;; Returns the result of the `line_delete_before` operation.
fn line_delete_before(str line, int col) str {
   "Deletes the whole character before byte column `col`."
   if col <= 0 { return line }
   def int end = char_snap(line, col)
   str.str_slice(line, 0, char_prev(line, end)) + str.str_slice(line, end, line.len)
}

fn insert_text(dict st, str text) dict {
   "Inserts text at cursor, preserving pasted newlines."
   if text.len <= 0 { return st }
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = _line_at(lines, row)
   def parts = split_lines(text)
   if parts.len <= 1 {
      lines[row] = line_insert(line, col, text)
      st["cursor_col"] = __box_i64(col + str.utf8_len(text))
      return set_lines(st, lines)
   }
   def before = str.str_slice(line, 0, col)
   def after = str.str_slice(line, col, line.len)
   mut out = _append_lines([], lines, 0, row)
   out = out.append(before + to_str(parts.get(0, "")))
   mut i = 1
   while i < parts.len - 1 {
      out = out.append(to_str(parts.get(i, "")))
      i += 1
   }
   def tail = to_str(parts.get(parts.len - 1, ""))
   out = out.append(tail + after)
   out = _append_lines(out, lines, row + 1, lines.len)
   st["cursor_line"] = __box_i64(row + parts.len - 1)
   st["cursor_col"] = __box_i64(str.utf8_len(tail))
   set_lines(st, out)
}

;; Returns the result of the `newline` operation.
fn newline(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = _line_at(lines, row)
   mut out = _append_lines([], lines, 0, row)
   out = out.append(str.str_slice(line, 0, col))
   out = out.append(str.str_slice(line, col, line.len))
   out = _append_lines(out, lines, row + 1, lines.len)
   st["cursor_line"] = __box_i64(row + 1)
   st["cursor_col"] = 0
   set_lines(st, out)
}

;; Returns the result of the `backspace` operation.
fn backspace(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   if col > 0 {
      def line = _line_at(lines, row)
      def int at = char_prev(line, col)
      lines[row] = str.str_slice(line, 0, at) + str.str_slice(line, col, line.len)
      st["cursor_col"] = __box_i64(at)
      return set_lines(st, lines)
   }
   if row <= 0 { return st }
   def prev = _line_at(lines, row - 1)
   def cur = _line_at(lines, row)
   mut out = _append_lines([], lines, 0, row - 1)
   out = out.append(prev + cur)
   out = _append_lines(out, lines, row + 1, lines.len)
   st["cursor_line"] = __box_i64(row - 1)
   st["cursor_col"] = __box_i64(str.utf8_len(prev))
   set_lines(st, out)
}

;; Releases the char.
fn delete_char(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = _line_at(lines, row)
   if col < str.utf8_len(line) {
      lines[row] = str.str_slice(line, 0, col) + str.str_slice(line, char_next(line, col), line.len)
      return set_lines(st, lines)
   }
   if row + 1 >= lines.len { return st }
   mut out = _append_lines([], lines, 0, row)
   out = out.append(line + _line_at(lines, row + 1))
   out = _append_lines(out, lines, row + 2, lines.len)
   set_lines(st, out)
}

;; Returns the result of the `switch_buffer` operation.
fn switch_buffer(dict st, int dir) dict {
   def bs = st.get("buffers", [])
   if bs.len <= 0 { return st }
   st["active"] = (int(st.get("active", 0)) + dir + bs.len) % bs.len
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["scroll"] = 0
   st
}

;; Updates the buffer and returns the resulting state.
fn select_buffer(dict st, int idx) dict {
   def bs = st.get("buffers", [])
   if idx < 0 || idx >= bs.len { return st }
   st["active"] = idx
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["sel_active"] = false
   st["scroll"] = 0
   st
}
