;; Keywords: editor module app text ui os render viewer
;; Public editor module wiring buffers, panels, commands, and UI drawing.
;; References:
;; - std.os.ui.render.viewer.editor.core
;; - std.os.ui.render.viewer.editor.commands
module std.os.ui.render.viewer.editor(
   LINE_H, STATUS_H,
   split_lines, join_lines, buffer, state,
   current_buffer, current_lines, set_lines, clamp_cursor,
   line_insert, line_delete_before, insert_text, newline, backspace, delete_char,
   switch_buffer, select_buffer, set_selection_anchor, update_selection,
   selection_range, selection_valid, selection_text, delete_selection,
   editor_layout, visible_rows,
   with_bottom_dock, pane_at, focus_next,
   row_at, col_at, buffer_at, divider_hit
)

use std.core
use std.os.ui.render.viewer.editor.core as core
use std.os.ui.render.viewer.editor.edit as edit
use std.os.ui.render.viewer.editor.selection as sel
use std.os.ui.render.viewer.editor.geom as geom
use std.os.ui.render.viewer.editor.keychord

def LINE_H = geom.LINE_H
def STATUS_H = geom.STATUS_H

fn split_lines(str text) list { core.split_lines(text) }

fn join_lines(list lines) str { core.join_lines(lines) }

fn buffer(str name, str path, str text) dict { core.buffer(name, path, text) }

fn state(list buffers) dict { core.state(buffers) }

fn current_buffer(dict st) dict { core.current_buffer(st) }

fn current_lines(dict st) list { core.current_lines(st) }

fn set_lines(dict st, list lines) dict { core.set_lines(st, lines) }

fn clamp_cursor(dict st) dict { core.clamp_cursor(st) }

fn line_insert(str line, int col, str text) str { edit.line_insert(line, col, text) }

fn line_delete_before(str line, int col) str { edit.line_delete_before(line, col) }

fn insert_text(dict st, str text) dict { edit.insert_text(st, text) }

fn newline(dict st) dict { edit.newline(st) }

fn backspace(dict st) dict { edit.backspace(st) }

fn delete_char(dict st) dict { edit.delete_char(st) }

fn switch_buffer(dict st, int dir) dict { edit.switch_buffer(st, dir) }

fn select_buffer(dict st, int idx) dict { edit.select_buffer(st, idx) }

fn set_selection_anchor(dict st) dict { sel.set_selection_anchor(st) }

fn update_selection(dict st) dict { sel.update_selection(st) }

fn selection_range(dict st) list { sel.selection_range(st) }

fn selection_valid(dict st) bool { sel.selection_valid(st) }

fn selection_text(dict st) str { sel.selection_text(st) }

fn delete_selection(dict st) dict { sel.delete_selection(st) }

fn editor_layout(f64 sw, f64 sh, f64 rail_w=250.0, f64 top_h=32.0) dict { geom.editor_layout(sw, sh, rail_w, top_h) }

fn with_bottom_dock(dict lay, f64 sh, bool open, f64 want_h=0.0) dict { geom.with_bottom_dock(lay, sh, open, want_h) }

fn pane_at(dict lay, f64 x, f64 y) str { geom.pane_at(lay, x, y) }

fn focus_next(str current, bool project_on=true, bool dock_on=false) str { geom.focus_next(current, project_on, dock_on) }

fn visible_rows(f64 edit_h) int { geom.visible_rows(edit_h) }

fn row_at(dict lay, f64 y, int scroll) int { geom.row_at(lay, y, scroll) }

fn col_at(any font, str line, f64 x, f64 text_x) int { geom.col_at(font, line, x, text_x) }

fn buffer_at(dict lay, f64 x, f64 y, int count) int { geom.buffer_at(lay, x, y, count) }

fn divider_hit(dict lay, f64 x, f64 y) bool { geom.divider_hit(lay, x, y) }

#main {
   mut st = state([buffer("a", "", "one\ntwo")])
   st = insert_text(st, "X")
   assert(join_lines(current_lines(st)) == "Xone\ntwo", "editor insert")
   st = newline(st)
   st = insert_text(st, "Y")
   assert(current_lines(st).get(1) == "Yone", "editor newline")
   st = backspace(st)
   assert(current_lines(st).get(0) == "X", "editor backspace merge")
   st = delete_char(st)
   assert(current_lines(st).get(1) == "ne", "editor delete")
   def lay = editor_layout(800.0, 480.0, 220.0)
   assert(visible_rows(float(lay.get("edit_h", 0.0))) > 0 && buffer_at(lay, float(lay.get("rail_x", 0.0)) + 20.0, float(lay.get("rail_y", 0.0)) + 50.0, 2) == 0, "editor layout hit")
   st["sel_a_line"] = 0
   st["sel_a_col"] = 0
   st["sel_b_line"] = 1
   st["sel_b_col"] = 1
   st["sel_active"] = true
   assert(selection_text(st) == "X\nn", "editor selection")
   st = delete_selection(st)
   assert(join_lines(current_lines(st)) == "e\ntwo" && int(st.get("cursor_line", -1)) == 0 && int(st.get("cursor_col", -1)) == 0, "editor delete selection")
   print("✓ viewer editor test passed")
}
