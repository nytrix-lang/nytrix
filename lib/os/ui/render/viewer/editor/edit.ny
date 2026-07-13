;; Keywords: editor edit text insert delete indent os ui render viewer
;; Text edit primitives for insertion, deletion, indentation, and line operations.
;; References:
;; - std.os.ui.render.viewer.editor.history
module std.os.ui.render.viewer.editor.edit(
   line_insert, line_delete_before, insert_text, newline, backspace, delete_char,
   switch_buffer, select_buffer
)

use std.core
use std.core.str as str
use std.math (max, min)
use std.os.ui.render.viewer.editor.core

@inline
fn _line_at(list lines, int i) str {
   to_str(lines.get(i, ""))
}

fn _copy_lines(list src, int a, int b, list dst, int at) int {
   mut i = a
   mut j = at
   while i < b {
      __store_item_fast(dst, j, src.get(i, ""))
      i += 1
      j += 1
   }
   j
}

fn _new_lines(int n) list {
   mut out = list(max(1, n))
   __list_set_len(out, max(1, n))
   out
}

;; Returns the result of the `line_insert` operation.
fn line_insert(str line, int col, str text) str {
   if text.len <= 0 { return line }
   col = min(max(col, 0), line.len)
   str.str_slice(line, 0, col) + text + str.str_slice(line, col, line.len)
}

;; Returns the result of the `line_delete_before` operation.
fn line_delete_before(str line, int col) str {
   col <= 0 ? line : str.str_slice(line, 0, min(col - 1, line.len)) + str.str_slice(line, min(col, line.len), line.len)
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
      st["cursor_col"] = col + text.len
      return set_lines(st, lines)
   }
   def before = str.str_slice(line, 0, col)
   def after = str.str_slice(line, col, line.len)
   def out_len = lines.len + parts.len - 1
   mut out = _new_lines(out_len)
   mut j = _copy_lines(lines, 0, row, out, 0)
   __store_item_fast(out, j, before + to_str(parts.get(0, "")))
   j += 1
   mut i = 1
   while i < parts.len - 1 {
      __store_item_fast(out, j, parts.get(i, ""))
      i += 1
      j += 1
   }
   def tail = to_str(parts.get(parts.len - 1, ""))
   __store_item_fast(out, j, tail + after)
   j += 1
   _copy_lines(lines, row + 1, lines.len, out, j)
   st["cursor_line"] = row + parts.len - 1
   st["cursor_col"] = tail.len
   set_lines(st, out)
}

;; Returns the result of the `newline` operation.
fn newline(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = _line_at(lines, row)
   mut out = _new_lines(lines.len + 1)
   mut j = _copy_lines(lines, 0, row, out, 0)
   __store_item_fast(out, j, str.str_slice(line, 0, col))
   __store_item_fast(out, j + 1, str.str_slice(line, col, line.len))
   _copy_lines(lines, row + 1, lines.len, out, j + 2)
   st["cursor_line"] = row + 1
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
      lines[row] = line_delete_before(to_str(lines.get(row, "")), col)
      st["cursor_col"] = col - 1
      return set_lines(st, lines)
   }
   if row <= 0 { return st }
   def prev = _line_at(lines, row - 1)
   def cur = _line_at(lines, row)
   mut out = _new_lines(lines.len - 1)
   mut j = _copy_lines(lines, 0, row - 1, out, 0)
   __store_item_fast(out, j, prev + cur)
   _copy_lines(lines, row + 1, lines.len, out, j + 1)
   st["cursor_line"] = row - 1
   st["cursor_col"] = prev.len
   set_lines(st, out)
}

;; Releases the char.
fn delete_char(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = _line_at(lines, row)
   if col < line.len {
      lines[row] = str.str_slice(line, 0, col) + str.str_slice(line, col + 1, line.len)
      return set_lines(st, lines)
   }
   if row + 1 >= lines.len { return st }
   mut out = _new_lines(lines.len - 1)
   mut j = _copy_lines(lines, 0, row, out, 0)
   __store_item_fast(out, j, line + _line_at(lines, row + 1))
   _copy_lines(lines, row + 2, lines.len, out, j + 1)
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
