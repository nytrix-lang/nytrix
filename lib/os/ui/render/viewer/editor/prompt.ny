;; Keywords: editor prompt input modal command os ui render viewer text
;; Prompt and inline input helpers for editor commands.
;; References:
;; - std.os.ui.render.viewer.editor.commands
module std.os.ui.render.viewer.editor.prompt(
   context_actions, context_actions_for, context_state, context_open, context_close, context_is_open,
   context_entry, context_x, context_y, context_clamp, context_hit, context_hover,
   rename_state, rename_start, rename_close, rename_is_open,
   rename_entry, rename_text, rename_rel, rename_handle_key, rename_handle_char
)

use std.core
use std.core.str as str
use std.math (max, min)
use std.os.ui.window
use std.os.ui.window.consts as key

def _FILE_ACTIONS = [
   ["Open", "open", "RET", "file", "Open the selected file in a buffer."],
   ["Run selection/section", "run", "C-SPC r r", "run", "Run the selected text, current section, or this file."],
   ["Check file", "check", "C-SPC r c", "run", "Compile or check this file."],
   ["Format file", "format", "C-SPC r f", "run", "Format this file when a formatter is available."],
   ["Git diff", "git-diff", "D", "diff", "Open the Git diff for this file."],
   ["Diff clipboard", "diff-clipboard", "C-SPC d c", "diff", "Compare this file with clipboard text."],
   ["Copy path", "copy-path", "C-SPC y p", "file", "Copy this path to the system clipboard."],
   ["Copy relative path", "copy-relative-path", "C-SPC y r", "file", "Copy this path relative to the project root."],
   ["Rename", "rename", "F2", "project", "Rename the selected file or directory."],
   ["Delete to Trash", "delete-file", "C-SPC f d", "file", "Move this file to project trash so it can be restored."],
   ["Undo file op", "undo-file-op", "C-SPC f u", "file", "Undo the latest file create/move/delete operation."],
   ["Redo file op", "redo-file-op", "C-SPC f R", "file", "Redo the latest reverted file operation."],
   ["Refresh", "refresh", "C-SPC g", "project", "Reload the tree and Git state."],
]

def _DIR_ACTIONS = [
   ["Open folder", "open", "RET", "file", "Expand or collapse this directory."],
   ["New file here", "new-file", "N", "file", "Create a file inside this directory."],
   ["Copy path", "copy-path", "C-SPC y p", "file", "Copy this directory path."],
   ["Copy relative path", "copy-relative-path", "C-SPC y r", "file", "Copy this directory path relative to the project root."],
   ["Rename", "rename", "F2", "project", "Rename the selected directory."],
   ["Delete to Trash", "delete-file", "C-SPC f d", "file", "Move this directory to project trash so it can be restored."],
   ["Undo file op", "undo-file-op", "C-SPC f u", "file", "Undo the latest file create/move/delete operation."],
   ["Redo file op", "redo-file-op", "C-SPC f R", "file", "Redo the latest reverted file operation."],
   ["Refresh", "refresh", "C-SPC g", "project", "Reload the tree and Git state."],
]

def _GIT_ACTIONS = [
   ["Open diff", "git-diff", "RET", "diff", "Show a unified diff for this changed file."],
   ["Open file", "open", "O", "file", "Open the changed file in a buffer."],
   ["Diff clipboard", "diff-clipboard", "C-SPC d c", "diff", "Compare this file against clipboard text."],
   ["Copy path", "copy-path", "C-SPC y p", "file", "Copy the changed file path."],
   ["Copy relative path", "copy-relative-path", "C-SPC y r", "file", "Copy the changed file path relative to the project root."],
   ["Delete to Trash", "delete-file", "C-SPC f d", "file", "Move this changed file to project trash so it can be restored."],
   ["Refresh", "refresh", "C-SPC g", "project", "Refresh Git status and file tree."],
]

def _TIMELINE_ACTIONS = [
   ["Open", "open", "RET", "file", "Open this recent file or replay this command."],
   ["Copy path", "copy-path", "C-SPC y p", "file", "Copy this timeline path."],
   ["Copy relative path", "copy-relative-path", "C-SPC y r", "file", "Copy this timeline path relative to the project root."],
   ["Refresh", "refresh", "C-SPC g", "project", "Refresh project history panels."],
]

def _EDITOR_ACTIONS = [
   ["Go to definition", "go-definition", "F10", "lsp", "Jump to the symbol under the cursor."],
   ["Hover", "lsp-hover", "C-i", "lsp", "Request hover/help for the symbol at point."],
   ["Complete", "lsp-complete", "C-.", "lsp", "Open completion candidates at point."],
   ["Diagnostics", "lsp-diagnostics", "C-SPC l d", "lsp", "Run diagnostics and show them in a buffer."],
   ["Run selection/section", "run", "C-SPC r r", "run", "Run selected text, the current section, or the current file."],
   ["Check file", "check", "C-SPC r c", "run", "Compile/check the current file."],
   ["Format file", "format", "C-SPC r f", "run", "Format the current file and reload it."],
   ["Copy", "copy", "C-c", "edit", "Copy selection, or the whole buffer if nothing is selected."],
   ["Cut", "cut", "C-x", "edit", "Cut the current selection."],
   ["Paste", "paste", "C-v", "edit", "Paste clipboard text at the cursor."],
   ["Toggle comment", "toggle-comment", "C-SPC e /", "edit", "Comment or uncomment the current selection."],
   ["Diff clipboard", "diff-clipboard", "C-SPC d c", "diff", "Compare this buffer with clipboard text."],
   ["Copy path", "copy-path", "C-SPC y p", "file", "Copy this buffer path."],
   ["Copy relative path", "copy-relative-path", "C-SPC y r", "file", "Copy this buffer path relative to the project root."],
   ["Copy line", "copy-line-info", "C-SPC y l", "file", "Copy path:line:column for the cursor."],
]

def _OUTLINE_ACTIONS = [
   ["Go to symbol", "open", "RET", "buffer", "Jump to the selected symbol."],
   ["Copy line", "copy-line-info", "C-SPC y l", "file", "Copy path:line:column for this symbol."],
   ["Go to definition", "go-definition", "F10", "lsp", "Resolve the symbol definition."],
]

def _TERMINAL_ACTIONS = [
   ["New shell tab", "terminal-shell", "F6", "terminal", "Open another shell tab."],
   ["Nytrix REPL", "terminal-repl", "F7", "terminal", "Open a Nytrix REPL tab."],
   ["Next terminal", "terminal-next", "C-SPC t ]", "terminal", "Focus the next terminal tab."],
   ["Previous terminal", "terminal-prev", "C-SPC t [", "terminal", "Focus the previous terminal tab."],
   ["Close terminal", "terminal-close", "C-SPC t x", "terminal", "Close the active terminal tab."],
   ["Focus editor", "pane-editor", "C-SPC w e", "pane", "Return focus to the editor buffer."],
]

fn context_actions() list { _FILE_ACTIONS }

fn context_actions_for(dict st) list {
   def scope = to_str(context_entry(st).get("scope", "file"))
   if scope == "editor" { return _EDITOR_ACTIONS }
   if scope == "git" { return _GIT_ACTIONS }
   if scope == "timeline" { return _TIMELINE_ACTIONS }
   if scope == "outline" { return _OUTLINE_ACTIONS }
   if scope == "terminal" { return _TERMINAL_ACTIONS }
   if bool(context_entry(st).get("dir", false)) { return _DIR_ACTIONS }
   _FILE_ACTIONS
}

fn context_state() dict {
   {"open": false, "x": 0.0, "y": 0.0, "entry": dict(8)}
}

fn context_open(dict st, f64 x, f64 y, dict entry) dict {
   st["open"] = true
   st["x"] = x
   st["y"] = y
   st["entry"] = entry
   st
}

fn context_close(dict st) dict {
   st["open"] = false
   st
}

fn context_is_open(dict st) bool { bool(st.get("open", false)) }

fn context_entry(dict st) dict { st.get("entry", dict(8)) }

fn context_x(dict st) f64 { float(st.get("x", 0.0)) }

fn context_y(dict st) f64 { float(st.get("y", 0.0)) }

fn context_clamp(dict st, f64 sw, f64 sh, f64 w=156.0, f64 row_h=24.0) dict {
   def actions = context_actions_for(st)
   def ww = min(max(1.0, w), max(1.0, sw - 16.0))
   def hh = min(max(1.0, float(actions.len) * row_h), max(1.0, sh - 16.0))
   st["x"] = max(8.0, min(context_x(st), max(8.0, sw - ww - 8.0)))
   st["y"] = max(8.0, min(context_y(st), max(8.0, sh - hh - 8.0)))
   st
}

fn context_hit(dict st, f64 mx, f64 my, f64 w=156.0, f64 row_h=24.0) str {
   if !context_is_open(st) { return "" }
   def x = context_x(st)
   def y = context_y(st)
   def actions = context_actions_for(st)
   if mx < x || mx > x + w || my < y || my > y + float(actions.len) * row_h { return "" }
   def idx = int((my - y) / row_h)
   if idx < 0 || idx >= actions.len { return "" }
   def row = actions.get(idx, [])
   is_list(row) ? to_str(row.get(1, "")) : ""
}

fn context_hover(dict st, f64 mx, f64 my, f64 w=156.0, f64 row_h=24.0) dict {
   if !context_is_open(st) { return dict(0) }
   def x = context_x(st)
   def y = context_y(st)
   def actions = context_actions_for(st)
   if mx < x || mx > x + w || my < y || my > y + float(actions.len) * row_h { return dict(0) }
   def idx = int((my - y) / row_h)
   if idx < 0 || idx >= actions.len { return dict(0) }
   def row = actions.get(idx)
   {"idx": idx, "id": to_str(row.get(1, "")), "title": to_str(row.get(0, "")), "key": to_str(row.get(2, "")), "tag": to_str(row.get(3, "")), "detail": to_str(row.get(4, ""))}
}

fn rename_state() dict {
   {"open": false, "text": "", "entry": dict(8)}
}

fn rename_start(dict st, dict entry) dict {
   st["open"] = true
   st["entry"] = entry
   st["text"] = to_str(entry.get("name", ""))
   st
}

fn rename_close(dict st) dict {
   st["open"] = false
   st["text"] = ""
   st
}

fn rename_is_open(dict st) bool { bool(st.get("open", false)) }

fn rename_entry(dict st) dict { st.get("entry", dict(8)) }

fn rename_text(dict st) str { to_str(st.get("text", "")) }

fn rename_rel(dict st) str { to_str(rename_entry(st).get("rel", "")) }

fn rename_handle_key(dict st, any data) dict {
   mut submit = false
   if window.event_key_is(data, key.KEY_ESCAPE) { st = rename_close(st) }
   elif window.event_key_is(data, key.KEY_ENTER) { submit = true }
   elif window.event_key_is(data, key.KEY_BACKSPACE) {
      def text = rename_text(st)
      if text.len > 0 { st["text"] = str.str_slice(text, 0, text.len - 1) }
   }
   {"st": st, "submit": submit}
}

fn rename_handle_char(dict st, any data) dict {
   def mods = int(data.get("mods", data.get("mod", 0)))
   if (mods & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0 { return st }
   def cp = int(data.get("char", 0))
   if cp >= 32 && cp != 127 { st["text"] = rename_text(st) + chr(cp) }
   st
}

#main {
   mut ctx = context_open(context_state(), 10.0, 20.0, {"name": "a"})
   assert(context_hit(ctx, 11.0, 21.0) == "open", "context hit")
   assert(to_str(context_hover(ctx, 11.0, 21.0).get("detail", "")).len > 0, "context hover detail")
   mut rn = rename_start(rename_state(), {"name": "old", "rel": "old"})
   rn = rename_handle_char(rn, {"char": 120})
   assert(rename_text(rn) == "oldx", "rename input")
}
