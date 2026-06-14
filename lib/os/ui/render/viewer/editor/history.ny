;; Keywords: editor history undo redo edit os ui render viewer text
;; Undo and redo history storage for editor text edits.
;; References:
;; - std.os.ui.render.viewer.editor.edit
module std.os.ui.render.viewer.editor.history(
   new, snapshot, push, undo, redo,
   insert_text, newline, backspace, delete_char, cut_selection, insert_pair
)

use std.core
use std.core.str as str
use std.os.ui.render.viewer.editor as ed

fn new() dict {
   {"undo": [], "redo": []}
}

fn _clone_buffer(any b) dict {
   mut out = clone(b)
   if is_dict(out) {
      def lines = out.get("lines", [""])
      out["lines"] = is_list(lines) ? clone(lines) : [to_str(lines)]
      return out
   }
   dict(8)
}

fn _clone_buffers(list buffers) list {
   mut out = []
   mut i = 0
   while i < buffers.len {
      out = out.append(_clone_buffer(buffers.get(i, {})))
      i += 1
   }
   out
}

fn snapshot(dict st) dict {
   {
      "buffers": _clone_buffers(st.get("buffers", [])),
      "active": int(st.get("active", 0)),
      "cursor_line": int(st.get("cursor_line", 0)),
      "cursor_col": int(st.get("cursor_col", 0)),
      "extra_cursors": clone(st.get("extra_cursors", [])),
      "multi_selects": clone(st.get("multi_selects", [])),
      "scroll": int(st.get("scroll", 0)),
      "rail_w": float(st.get("rail_w", 250.0)),
   }
}

fn _same(dict a, dict b) bool {
   a.get("buffers", []) == b.get("buffers", []) &&
   int(a.get("active", 0)) == int(b.get("active", 0)) &&
   int(a.get("cursor_line", 0)) == int(b.get("cursor_line", 0)) &&
   int(a.get("cursor_col", 0)) == int(b.get("cursor_col", 0)) &&
   a.get("extra_cursors", []) == b.get("extra_cursors", []) &&
   a.get("multi_selects", []) == b.get("multi_selects", [])
}

fn _restore(dict st, dict snap) dict {
   st["buffers"] = _clone_buffers(snap.get("buffers", st.get("buffers", [])))
   st["active"] = int(snap.get("active", 0))
   st["cursor_line"] = int(snap.get("cursor_line", 0))
   st["cursor_col"] = int(snap.get("cursor_col", 0))
   st["extra_cursors"] = clone(snap.get("extra_cursors", []))
   st["multi_selects"] = clone(snap.get("multi_selects", []))
   st["scroll"] = int(snap.get("scroll", 0))
   st["rail_w"] = float(snap.get("rail_w", 250.0))
   st["sel_active"] = false
   st
}

fn push(dict hist, dict st) dict {
   def snap = snapshot(st)
   mut undo = hist.get("undo", [])
   if undo.len > 0 && _same(undo.get(undo.len - 1), snap) { return hist }
   undo = undo.append(snap)
   if undo.len > 128 { undo = slice(undo, undo.len - 128, undo.len, 1) }
   hist["undo"] = undo
   hist["redo"] = []
   hist
}

fn _pack(dict hist, dict st, str status="") dict {
   {"hist": hist, "st": st, "status": status}
}

fn undo(dict hist, dict st) dict {
   mut undo = hist.get("undo", [])
   if undo.len <= 0 { return _pack(hist, st, "nothing to undo") }
   mut redo = hist.get("redo", [])
   redo = redo.append(snapshot(st))
   def snap = undo.get(undo.len - 1)
   undo = slice(undo, 0, undo.len - 1, 1)
   hist["undo"] = undo
   hist["redo"] = redo
   _pack(hist, _restore(st, snap), "undo")
}

fn redo(dict hist, dict st) dict {
   mut redo = hist.get("redo", [])
   if redo.len <= 0 { return _pack(hist, st, "nothing to redo") }
   mut undo = hist.get("undo", [])
   undo = undo.append(snapshot(st))
   def snap = redo.get(redo.len - 1)
   redo = slice(redo, 0, redo.len - 1, 1)
   hist["undo"] = undo
   hist["redo"] = redo
   _pack(hist, _restore(st, snap), "redo")
}

fn _before_edit(dict hist, dict st) dict {
   hist = push(hist, st)
   if ed.selection_valid(st) { st = ed.delete_selection(st) }
   {"hist": hist, "st": st}
}

fn insert_text(dict hist, dict st, str text) dict {
   if text.len <= 0 { return _pack(hist, st) }
   def p = _before_edit(hist, st)
   hist = p.get("hist", hist)
   st = ed.insert_text(p.get("st", st), text)
   _pack(hist, st)
}

fn newline(dict hist, dict st) dict {
   def p = _before_edit(hist, st)
   _pack(p.get("hist", hist), ed.newline(p.get("st", st)))
}

fn backspace(dict hist, dict st) dict {
   hist = push(hist, st)
   if ed.selection_valid(st) { st = ed.delete_selection(st) }
   else { st = ed.backspace(st) }
   _pack(hist, st)
}

fn delete_char(dict hist, dict st) dict {
   hist = push(hist, st)
   if ed.selection_valid(st) { st = ed.delete_selection(st) }
   else { st = ed.delete_char(st) }
   _pack(hist, st)
}

fn cut_selection(dict hist, dict st) dict {
   if !ed.selection_valid(st) { return _pack(hist, st, "no selection") }
   hist = push(hist, st)
   _pack(hist, ed.delete_selection(st), "cut")
}

fn insert_pair(dict hist, dict st, str open, str close) dict {
   hist = push(hist, st)
   def selected = ed.selection_text(st)
   if selected.len > 0 {
      st = ed.delete_selection(st)
      st = ed.insert_text(st, open + selected + close)
      return _pack(hist, st)
   }
   st = ed.insert_text(st, open + close)
   st["cursor_col"] = int(st.get("cursor_col", 0)) - close.len
   _pack(hist, ed.clamp_cursor(st))
}

#main {
   mut hist = new()
   mut st = ed.state([ed.buffer("a", "", "one")])
   def r = insert_text(hist, st, "x")
   hist = r.get("hist", hist)
   st = r.get("st", st)
   assert(ed.join_lines(ed.current_lines(st)) == "xone", "history insert")
   def u = undo(hist, st)
   assert(to_str(u.get("status", "")) == "undo" && ed.join_lines(ed.current_lines(u.get("st", st))) == "one", "history undo")
}
