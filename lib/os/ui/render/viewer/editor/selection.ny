;; Keywords: editor selection cursor range text os ui render viewer
;; Selection and cursor range helpers for text editing operations.
;; References:
;; - std.os.ui.render.viewer.editor.core
module std.os.ui.render.viewer.editor.selection(
   set_selection_anchor, update_selection, selection_range,
   selection_valid, selection_text, delete_selection
)

use std.core
use std.core.str as str
use std.math (max, min)
use std.os.ui.render.viewer.editor.core

fn set_selection_anchor(dict st) dict {
   "Starts a selection at the current cursor."
   st["sel_active"] = false
   st["drag_select"] = true
   st["sel_a_line"] = int(st.get("cursor_line", 0))
   st["sel_a_col"] = int(st.get("cursor_col", 0))
   st["sel_b_line"] = int(st.get("cursor_line", 0))
   st["sel_b_col"] = int(st.get("cursor_col", 0))
   st
}

fn update_selection(dict st) dict {
   "Extends the active selection to the current cursor."
   st["sel_active"] = true
   st["sel_b_line"] = int(st.get("cursor_line", 0))
   st["sel_b_col"] = int(st.get("cursor_col", 0))
   st
}

fn selection_range(dict st) list {
   "Returns [start_line, start_col, end_line, end_col] for selection painting."
   def al = int(st.get("sel_a_line", 0))
   def ac = int(st.get("sel_a_col", 0))
   def bl = int(st.get("sel_b_line", 0))
   def bc = int(st.get("sel_b_col", 0))
   if al > bl || (al == bl && ac > bc) { return [bl, bc, al, ac] }
   [al, ac, bl, bc]
}

fn selection_valid(dict st) bool {
   st.get("sel_active", false) && (
      int(st.get("sel_a_line", 0)) != int(st.get("sel_b_line", 0)) ||
      int(st.get("sel_a_col", 0)) != int(st.get("sel_b_col", 0))
   )
}

fn selection_text(dict st) str {
   "Returns selected text or an empty string."
   if !selection_valid(st) { return "" }
   def ord = selection_range(st)
   def sl = int(ord.get(0, 0))
   def sc = int(ord.get(1, 0))
   def el = int(ord.get(2, 0))
   def ec = int(ord.get(3, 0))
   def lines = current_lines(st)
   mut out = []
   mut i = sl
   while i <= el && i < lines.len {
      def line = to_str(lines.get(i, ""))
      def a = i == sl ? min(sc, line.len) : 0
      def b = i == el ? min(ec, line.len) : line.len
      out = out.append(str.str_slice(line, a, max(a, b)))
      i += 1
   }
   join_lines(out)
}

fn delete_selection(dict st) dict {
   "Deletes the active selection and moves the cursor to its start."
   if !selection_valid(st) { return st }
   def ord = selection_range(st)
   def sl = int(ord.get(0, 0))
   def sc = int(ord.get(1, 0))
   def el = int(ord.get(2, 0))
   def ec = int(ord.get(3, 0))
   def lines = current_lines(st)
   def first = to_str(lines.get(sl, ""))
   def last = to_str(lines.get(el, ""))
   mut out = []
   mut i = 0
   while i < sl { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(str.str_slice(first, 0, min(sc, first.len)) + str.str_slice(last, min(ec, last.len), last.len))
   i = el + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   st["cursor_line"] = sl
   st["cursor_col"] = sc
   st["sel_active"] = false
   set_lines(st, out)
}
