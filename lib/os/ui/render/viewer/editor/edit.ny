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
use std.math (max)
use std.os.ui.render.viewer.editor.core

fn line_insert(str line, int col, str text) str {
   str.str_slice(line, 0, col) + text + str.str_slice(line, col, line.len)
}

fn line_delete_before(str line, int col) str {
   col <= 0 ? line : str.str_slice(line, 0, col - 1) + str.str_slice(line, col, line.len)
}

fn insert_text(dict st, str text) dict {
   "Inserts text at cursor, preserving pasted newlines."
   if text.len <= 0 { return st }
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = to_str(lines.get(row, ""))
   def parts = split_lines(text)
   if parts.len <= 1 {
      lines[row] = line_insert(line, col, text)
      st["cursor_col"] = col + text.len
      return set_lines(st, lines)
   }
   def before = str.str_slice(line, 0, col)
   def after = str.str_slice(line, col, line.len)
   mut out = []
   mut i = 0
   while i < row { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(before + to_str(parts.get(0, "")))
   i = 1
   while i < parts.len - 1 { out = out.append(parts.get(i, "")) i += 1 }
   out = out.append(to_str(parts.get(parts.len - 1, "")) + after)
   i = row + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   st["cursor_line"] = row + parts.len - 1
   st["cursor_col"] = to_str(parts.get(parts.len - 1, "")).len
   set_lines(st, out)
}

fn newline(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = to_str(lines.get(row, ""))
   mut out = []
   mut i = 0
   while i < row { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(str.str_slice(line, 0, col))
   out = out.append(str.str_slice(line, col, line.len))
   i = row + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   st["cursor_line"] = row + 1
   st["cursor_col"] = 0
   set_lines(st, out)
}

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
   def prev = to_str(lines.get(row - 1, ""))
   def cur = to_str(lines.get(row, ""))
   mut out = []
   mut i = 0
   while i < row - 1 { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(prev + cur)
   i = row + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   st["cursor_line"] = row - 1
   st["cursor_col"] = prev.len
   set_lines(st, out)
}

fn delete_char(dict st) dict {
   st = clamp_cursor(st)
   mut lines = current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   def line = to_str(lines.get(row, ""))
   if col < line.len {
      lines[row] = str.str_slice(line, 0, col) + str.str_slice(line, col + 1, line.len)
      return set_lines(st, lines)
   }
   if row + 1 >= lines.len { return st }
   lines[row] = line + to_str(lines.get(row + 1, ""))
   mut out = []
   mut i = 0
   while i < lines.len { if i != row + 1 { out = out.append(lines.get(i, "")) } i += 1 }
   set_lines(st, out)
}

fn switch_buffer(dict st, int dir) dict {
   def bs = st.get("buffers", [])
   if bs.len <= 0 { return st }
   st["active"] = (int(st.get("active", 0)) + dir + bs.len) % bs.len
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["scroll"] = 0
   st
}

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
