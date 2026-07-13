;; Keywords: editor commands actions keymap palette os ui render viewer text
;; Command registry and action dispatch helpers for editor tools and keymaps.
;; References:
;; - std.os.ui.render.viewer.editor.keychord
module std.os.ui.render.viewer.editor.commands(
   commands, all_commands, enabled, with_tag, by_id, id_for_chord, which_key, has_id,
   config, set_enabled, is_enabled, toggle, toggles, row_id, row_key, row_tag
)

use std.core
use std.core.str as str

def _COMMANDS = [
   ["Save file", "save", "C-s", "write current buffer", "file"],
   ["Save file", "save", "C-SPC f s", "write current buffer", "file"],
   ["Save all", "save-all", "C-SPC f a", "write all file-backed buffers", "file"],
   ["Reload from disk", "reload", "C-SPC f r", "reload current file from disk", "file"],
   ["Copy file path", "copy-path", "C-SPC y p", "copy current file path", "file"],
   ["Copy relative path", "copy-relative-path", "C-SPC y r", "copy path relative to the project root", "file"],
   ["Copy line info", "copy-line-info", "C-SPC y l", "copy path:line:column", "file"],
   ["Open file", "open-file", "C-SPC f f", "open project file from command prompt", "file"],
   ["New project file", "new-file", "C-SPC f n", "create a file beside the selected/current file", "file"],
   ["Delete selected file", "delete-file", "C-SPC f d", "move selected/current file to project trash", "file"],
   ["Undo file operation", "undo-file-op", "C-SPC f u", "restore the latest trashed file or reverted file move", "file"],
   ["Redo file operation", "redo-file-op", "C-SPC f R", "redo the latest reverted file operation", "file"],
   ["New scratch", "new-scratch", "C-SPC b s", "create an unsaved scratch page", "buffer"],
   ["Kill buffer", "kill-buffer", "C-SPC b k", "close current buffer", "buffer"],
   ["Diff with clipboard", "diff-clipboard", "C-SPC d c", "compare buffer against clipboard side-by-side", "diff"],
   ["Run selection/section", "run", "C-SPC r r", "run selection, section, or current file", "run"],
   ["Run selection/section", "run", "C-c r r", "run selection, section, or current file", "run"],
   ["Check diagnostics", "check", "C-SPC r c", "compile/check current file", "run"],
   ["Check diagnostics", "check", "C-c r c", "compile/check current file", "run"],
   ["Format file", "format", "C-SPC r f", "format current file", "run"],
   ["Format file", "format", "C-c r f", "format current file", "run"],
   ["Debug start", "debug-start", "C-SPC d d", "start configured debugger", "debug"],
   ["Debug start", "debug-start", "C-c d d", "start configured debugger", "debug"],
   ["Debug start", "debug-start", "F8", "start configured debugger", "debug"],
   ["Debug breakpoint", "debug-breakpoint", "F9", "break at current line", "debug"],
   ["Debug breakpoint", "debug-breakpoint", "C-SPC d b", "break at current line", "debug"],
   ["Debug continue", "debug-continue", "F5", "continue debugger", "debug"],
   ["Debug continue", "debug-continue", "C-SPC d r", "continue debugger", "debug"],
   ["Debug next", "debug-next", "C-SPC d n", "step over", "debug"],
   ["Debug step", "debug-step", "C-SPC d s", "step into", "debug"],
   ["Debug finish", "debug-finish", "C-SPC d f", "finish current frame", "debug"],
   ["Debug backtrace", "debug-backtrace", "C-SPC d t", "print backtrace", "debug"],
   ["Debug until line", "debug-until-line", "C-SPC d u", "run until current line", "debug"],
   ["Debug jump to line", "debug-jump-line", "C-SPC d j", "jump target to current line", "debug"],
   ["Debug inspect line", "debug-inspect-line", "C-SPC d i", "print identifiers on current line", "debug"],
   ["Debug reverse continue", "debug-reverse-continue", "C-SPC d v", "reverse continue under rr/gdb record", "debug"],
   ["Debug reverse next", "debug-reverse-next", "C-SPC d p", "reverse next under rr/gdb record", "debug"],
   ["Debug reverse step", "debug-reverse-step", "C-SPC d a", "reverse step under rr/gdb record", "debug"],
   ["Debug load coredump", "debug-coredump", "C-SPC d k", "open latest coredump in gdb", "debug"],
   ["Refresh project", "refresh", "C-SPC g", "refresh tree and git status", "project"],
   ["Rename selected file", "rename", "F2", "rename tree selection", "project"],
   ["Find in buffer", "find", "C-f", "open search in the current buffer", "search"],
   ["Replace in buffer", "replace", "C-h", "open search and replace in the current buffer", "search"],
   ["Replace in buffer", "replace", "C-SPC s r", "open search and replace in the current buffer", "search"],
   ["Find next", "find-next", "F3", "jump to next search result", "search"],
   ["Find previous", "find-prev", "Shift-F3", "jump to previous search result", "search"],
   ["Replace current match", "replace-current", "C-SPC s RET", "replace the active search result", "search"],
   ["Replace all matches", "replace-all", "C-SPC s a", "replace all search results in the current buffer", "search"],
   ["Command palette", "palette", "M-x", "open command palette", "ui"],
   ["Command palette", "palette", "C-Shift-p", "open command palette", "ui"],
   ["Command palette", "palette", "C-SPC SPC", "open command palette", "ui"],
   ["Cancel", "cancel", "C-g", "cancel prompt/selection", "ui"],
   ["Quit prompt", "quit", "ESC", "ask before closing", "ui"],
   ["Undo", "undo", "C-z", "undo edit", "edit"],
   ["Undo", "undo", "C-/", "undo edit", "edit"],
   ["Undo", "undo", "C-_", "undo edit", "edit"],
   ["Redo", "redo", "C-Shift-z", "redo edit", "edit"],
   ["Redo", "redo", "C-y", "redo edit", "edit"],
   ["Copy", "copy", "C-c", "copy selection or buffer", "edit"],
   ["Copy", "copy", "M-w", "copy selection or buffer", "edit"],
   ["Cut", "cut", "C-x", "cut selection", "edit"],
   ["Cut", "cut", "C-w", "cut selection", "edit"],
   ["Paste", "paste", "C-v", "paste clipboard", "edit"],
   ["Mark whole buffer", "mark-whole", "C-a", "select all text", "edit"],
   ["Mark whole buffer", "mark-whole", "C-SPC e a", "select all text", "edit"],
   ["Select line", "select-line", "C-l", "select current line", "edit"],
   ["Kill line", "kill-line", "C-k", "cut to end of line", "edit"],
   ["Duplicate line", "duplicate-line", "C-Shift-d", "duplicate current line", "edit"],
   ["Delete line", "delete-line", "C-Shift-k", "delete current line", "edit"],
   ["Move line up", "move-line-up", "M-UP", "move current line upward", "edit"],
   ["Move line down", "move-line-down", "M-DOWN", "move current line downward", "edit"],
   ["Join lines", "join-lines", "C-j", "join current line with next", "edit"],
   ["Join lines tight", "join-lines-tight", "C-Shift-j", "join without inserted space", "edit"],
   ["Sort lines", "sort-lines", "C-SPC e s", "sort selected lines or buffer", "edit"],
   ["Toggle comment", "toggle-comment", "C-SPC e /", "comment or uncomment selected lines", "edit"],
   ["Strip trailing whitespace", "strip-trailing-whitespace", "C-SPC e w", "trim trailing spaces/tabs", "edit"],
   ["Color picker", "color-picker", "C-SPC e c", "inspect hex color under cursor", "edit"],
   ["Toggle sidebar", "toggle-sidebar", "C-b", "show/hide the file sidebar", "toggle"],
   ["Toggle sidebar", "toggle-sidebar", "C-SPC t b", "show/hide the file sidebar", "toggle"],
   ["Toggle title bar", "toggle-titlebar", "C-t", "show/hide the top title bar", "toggle"],
   ["Toggle title bar", "toggle-titlebar", "C-SPC t h", "show/hide the top title bar", "toggle"],
   ["Zoom in", "zoom-in", "C-+", "increase editor and terminal font size", "ui"],
   ["Zoom in", "zoom-in", "C-=", "increase editor and terminal font size", "ui"],
   ["Zoom out", "zoom-out", "C--", "decrease editor and terminal font size", "ui"],
   ["Zoom reset", "zoom-reset", "C-0", "reset editor and terminal font size", "ui"],
   ["Previous buffer", "prev", "C-SPC b p", "previous buffer", "buffer"],
   ["Previous tab", "prev", "C-Shift-TAB", "previous buffer page", "buffer"],
   ["Next buffer", "next", "C-SPC b n", "next buffer", "buffer"],
   ["Next tab", "next", "C-TAB", "next buffer page", "buffer"],
   ["Go back", "go-back", "M-LEFT", "previous editor location", "buffer"],
   ["Go forward", "go-forward", "M-RIGHT", "next editor location", "buffer"],
   ["Go to definition", "go-definition", "F10", "jump to symbol definition", "buffer"],
   ["Go to definition", "go-definition", "M-.", "jump to symbol definition", "buffer"],
   ["Next pane", "pane-next", "C-SPC w o", "focus next visible pane", "pane"],
   ["Focus project", "pane-project", "C-SPC w p", "focus project pane", "pane"],
   ["Focus editor", "pane-editor", "C-SPC w e", "focus editor pane", "pane"],
   ["Focus terminal", "pane-terminal", "C-SPC w t", "focus terminal pane", "pane"],
   ["Close pane", "pane-close", "C-SPC w 0", "close focused auxiliary pane", "pane"],
   ["Keep one pane", "pane-only", "C-SPC w 1", "hide auxiliary panes", "pane"],
   ["Split below", "pane-split-below", "C-SPC w 2", "open bottom terminal pane", "pane"],
   ["Split right", "pane-split-right", "C-SPC w 3", "show project pane", "pane"],
   ["Balance panes", "pane-balance", "C-SPC w =", "balance pane sizes", "pane"],
   ["Toggle fullscreen", "toggle-fullscreen", "C-SPC t f", "toggle fullscreen window", "ui"],
   ["Resize left", "pane-resize-left", "C-SPC w h", "shrink side pane", "pane"],
   ["Resize right", "pane-resize-right", "C-SPC w l", "grow side pane", "pane"],
   ["Resize up", "pane-resize-up", "C-SPC w k", "grow bottom dock", "pane"],
   ["Resize down", "pane-resize-down", "C-SPC w j", "shrink bottom dock", "pane"],
   ["Backspace", "backspace", "BACKSPACE", "delete before cursor", "edit"],
   ["Delete char", "delete-char", "DELETE", "delete under cursor", "edit"],
   ["New line", "newline", "RET", "insert newline", "edit"],
   ["Move left", "move-left", "LEFT", "move cursor left", "cursor"],
   ["Move right", "move-right", "RIGHT", "move cursor right", "cursor"],
   ["Move right", "move-right", "C-SPC m f", "move cursor right", "cursor"],
   ["Move up", "move-up", "UP", "move cursor up", "cursor"],
   ["Move up", "move-up", "C-p", "move cursor up", "cursor"],
   ["Move down", "move-down", "DOWN", "move cursor down", "cursor"],
   ["Move down", "move-down", "C-n", "move cursor down", "cursor"],
   ["Move word left", "move-left-fast", "C-LEFT", "move cursor left faster", "cursor"],
   ["Move word left", "move-left-fast", "M-b", "move cursor left faster", "cursor"],
   ["Move word right", "move-right-fast", "C-RIGHT", "move cursor right faster", "cursor"],
   ["Move word right", "move-right-fast", "M-f", "move cursor right faster", "cursor"],
   ["Move block up", "move-up-fast", "C-UP", "move cursor up faster", "cursor"],
   ["Move block down", "move-down-fast", "C-DOWN", "move cursor down faster", "cursor"],
   ["Move page up", "page-up", "PGUP", "move cursor up by page", "cursor"],
   ["Move page down", "page-down", "PGDN", "move cursor down by page", "cursor"],
   ["Line start", "line-start", "HOME", "move to line start", "cursor"],
   ["Line start", "line-start", "C-SPC m a", "move to line start", "cursor"],
   ["Line end", "line-end", "END", "move to line end", "cursor"],
   ["Line end", "line-end", "C-e", "move to line end", "cursor"],
   ["Buffer start", "buffer-start", "C-SPC m p", "move to start of buffer", "cursor"],
   ["Buffer end", "buffer-end", "C-SPC m n", "move to end of buffer", "cursor"],
   ["Add cursor above", "cursor-add-above", "C-M-UP", "add an aligned cursor above", "cursor"],
   ["Add cursor above", "cursor-add-above", "M-Shift-UP", "add an aligned cursor above", "cursor"],
   ["Add cursor below", "cursor-add-below", "C-M-DOWN", "add an aligned cursor below", "cursor"],
   ["Add cursor below", "cursor-add-below", "M-Shift-DOWN", "add an aligned cursor below", "cursor"],
   ["Clear extra cursors", "cursor-clear-extra", "C-SPC m c", "clear line cursors", "cursor"],
   ["Toggle project tree", "toggle-project", "C-SPC t p", "show/hide project tree", "toggle"],
   ["Toggle outline", "toggle-outline", "C-SPC t o", "show/hide outline", "toggle"],
   ["Toggle status bar", "toggle-status", "C-SPC t s", "show/hide status bar", "toggle"],
   ["Toggle line numbers", "toggle-line-numbers", "C-SPC t n", "show/hide gutter line numbers", "toggle"],
   ["Toggle indent guides", "toggle-indent-guides", "C-SPC t i", "show/hide indentation guides", "toggle"],
   ["Toggle terminal", "toggle-terminal", "C-SPC t t", "show/hide shell terminal", "toggle"],
   ["Toggle REPL", "toggle-repl", "C-SPC t r", "show/hide Nytrix REPL terminal", "toggle"],
   ["Toggle LSP popups", "toggle-lsp-popups", "F11", "enable/disable LSP popups", "toggle"],
   ["Terminal shell", "terminal-shell", "F6", "open shell terminal", "terminal"],
   ["Nytrix REPL", "terminal-repl", "F7", "open Nytrix REPL", "terminal"],
   ["Next terminal tab", "terminal-next", "C-SPC t ]", "focus next terminal tab", "terminal"],
   ["Previous terminal tab", "terminal-prev", "C-SPC t [", "focus previous terminal tab", "terminal"],
   ["Close terminal tab", "terminal-close", "C-SPC t x", "close active terminal tab", "terminal"],
   ["LSP status", "lsp-status", "C-SPC l s", "show language server status", "lsp"],
   ["LSP start", "lsp-start", "C-SPC l l", "start configured language server", "lsp"],
   ["LSP restart", "lsp-restart", "F12", "restart configured language server", "lsp"],
   ["LSP stop", "lsp-stop", "C-SPC l q", "stop language server", "lsp"],
   ["LSP diagnostics", "lsp-diagnostics", "C-SPC l d", "show diagnostics", "lsp"],
   ["Completion popup", "lsp-complete", "C-.", "request completions", "lsp"],
   ["Completion popup", "lsp-complete", "C-SPC l c", "request completions", "lsp"],
   ["LSP hover", "lsp-hover", "C-i", "request hover info", "lsp"],
   ["LSP hover", "lsp-hover", "C-SPC l h", "request hover info", "lsp"],
   ["LSP definition request", "lsp-definition", "C-SPC l g", "emit definition request", "lsp"],
]

def _ALIASES = []

;; Returns the result of the `commands` operation.
fn commands() list { _COMMANDS }

;; Returns the result of the `all_commands` operation.
fn all_commands() list { _COMMANDS }

fn row_id(any row) str { to_str(row.get(1, "")) }

fn row_key(any row) str { to_str(row.get(2, "")) }

fn row_tag(any row) str { to_str(row.get(4, "")) }

;; Returns the result of the `config` operation.
fn config() dict {
   {"file": true, "diff": true, "run": true, "debug": true, "project": true, "ui": true, "edit": true, "buffer": true, "cursor": true, "search": true, "pane": true, "toggle": true, "terminal": true, "lsp": true}
}

;; Returns true when is enabled.
fn is_enabled(dict cfg, any row) bool {
   cfg.get(row_tag(row), true)
}

;; Updates the enabled and returns the resulting state.
fn set_enabled(dict cfg, str tag, bool on) dict {
   cfg[tag] = on
   cfg
}

;; Returns the result of the `toggle` operation.
fn toggle(dict cfg, str tag) dict {
   set_enabled(cfg, tag, !bool(cfg.get(tag, true)))
}

;; Returns the result of the `enabled` operation.
fn enabled(dict cfg=config()) list {
   mut out = []
   mut i = 0
   while i < _COMMANDS.len {
      def row = _COMMANDS.get(i)
      if is_enabled(cfg, row) { out = out.append(row) }
      i += 1
   }
   out
}

;; Returns the result of the `with_tag` operation.
fn with_tag(str tag, dict cfg=config()) list {
   mut out = []
   def rows = enabled(cfg)
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if row_tag(row) == tag { out = out.append(row) }
      i += 1
   }
   out
}

;; Returns the result of the `toggles` operation.
fn toggles(dict cfg=config()) list {
   with_tag("toggle", cfg)
}

;; Returns the result of the `by_id` operation.
fn by_id(str id) list {
   def rows = commands()
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if row_id(row) == id { return row }
      i += 1
   }
   []
}

;; Returns true when has id.
fn has_id(str id) bool { by_id(id).len > 0 }

;; Returns the result of the `id_for_chord` operation.
fn id_for_chord(str chord) str {
   def rows = all_commands()
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if row_key(row) == chord { return row_id(row) }
      i += 1
   }
   ""
}

fn _suffix_head(str suffix) str {
   def pos = str.find(suffix, " ")
   pos < 0 ? suffix : str.str_slice(suffix, 0, pos)
}

fn _group_label(str head) str {
   if head == "SPC" { return "command palette" }
   if head == "f" { return "files" }
   if head == "b" { return "buffers and pages" }
   if head == "d" { return "debug and diff" }
   if head == "r" { return "run and check" }
   if head == "g" { return "project refresh" }
   if head == "y" { return "copy helpers" }
   if head == "e" { return "editing tools" }
   if head == "s" { return "search" }
   if head == "t" { return "toggles and terminal" }
   if head == "w" { return "windows and panes" }
   if head == "m" { return "movement" }
   if head == "l" { return "language server" }
   head
}

;; Finds the key and returns the matching result.
fn which_key(str prefix) list {
   def p = str.strip(prefix)
   mut out = []
   mut seen = dict(32)
   mut i = 0
   def rows = all_commands()
   while i < rows.len {
      def row = rows.get(i)
      def key = row_key(row)
      def pre = p + " "
      if str.startswith(key, pre) {
         def suffix = str.str_slice(key, pre.len, key.len)
         def head = _suffix_head(suffix)
         if head.len > 0 && !seen.contains(head) {
            seen[head] = true
            out = out.append([head, suffix == head ? to_str(row.get(0, "")) : _group_label(head)])
         }
      }
      i += 1
   }
   out
}

#main {
   assert(id_for_chord("C-SPC f s") == "save" && id_for_chord("C-c") == "copy" && id_for_chord("C-y") == "redo" && by_id("lsp-status").len > 0, "editor commands")
   mut cfg = config()
   cfg = set_enabled(cfg, "lsp", false)
   assert(which_key("C-SPC").len > 0 && with_tag("lsp", cfg).len == 0 && toggles().len > 0, "editor which-key")
}
