;; Keywords: editor commands actions keymap palette os ui render viewer text
;; Command registry and action dispatch helpers for editor tools and keymaps.
;; References:
;; - core.os.ui.render.viewer.editor.keychord
module core.os.ui.render.viewer.editor.commands(
   commands, all_commands, enabled, with_tag, by_id, id_for_chord, which_key, has_id,
   config, set_enabled, is_enabled, toggle, toggles, row_id, row_key, row_tag,
   keymap, set_keymap, keymap_names, bindings_for, describe_chord,
   rebind, unbind, user_bindings, parse_bindings, load_bindings_file
)

use core
use core.str as str
use core.os (env, file_read)

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
   ["Debug break_point", "debug-break_point", "F9", "break at current line", "debug"],
   ["Debug break_point", "debug-break_point", "C-SPC d b", "break at current line", "debug"],
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
   ["LSP definition", "lsp-definition", "C-SPC l g", "jump to definition via the language server", "lsp"],
   ["Find references", "lsp-references", "S-F12", "list every reference to the symbol", "lsp"],
   ["Find references", "lsp-references", "C-SPC l r", "list every reference to the symbol", "lsp"],
   ["Rename symbol", "lsp-rename", "C-SPC l n", "rename the symbol across the workspace", "lsp"],
   ["Format document", "lsp-format", "C-SPC l f", "format with the language server", "lsp"],
   ["Document symbols", "lsp-symbols", "C-SPC l o", "list symbols in this file", "lsp"],
   ["Code actions", "lsp-actions", "C-SPC l a", "apply the quick fix at the cursor", "lsp"],
   ["Signature help", "lsp-signature", "C-SPC l k", "show the call signature", "lsp"],
]

def _ALIASES = []

;; Emacs profile. These rows are added in front of the default table; default
;; rows whose chord is taken by an Emacs binding or prefix (C-x, C-h, C-y, ...)
;; are dropped, so every chord maps to exactly one command. Help lives under
;; the C-h prefix, buffers/windows/files under C-x, as in Emacs.
def _EMACS_COMMANDS = [
   ["Save", "save", "C-x C-s", "write the buffer to its file", "file"],
   ["Save all", "save-all", "C-x s", "write every modified buffer", "file"],
   ["Find file", "open-file", "C-x C-f", "open a file", "file"],
   ["Revert buffer", "reload", "C-x C-r", "reload the buffer from disk", "file"],
   ["Kill buffer", "kill-buffer", "C-x k", "close the current buffer", "buffer"],
   ["Next buffer", "next", "C-x RIGHT", "switch to the next buffer", "buffer"],
   ["Previous buffer", "prev", "C-x LEFT", "switch to the previous buffer", "buffer"],
   ["Switch buffer", "switch-buffer", "C-x b", "fuzzy-pick an open buffer", "buffer"],
   ["List buffers", "switch-buffer", "C-x C-b", "fuzzy-pick an open buffer", "buffer"],
   ["Other window", "pane-next", "C-x o", "move focus to the next pane", "pane"],
   ["Delete window", "pane-close", "C-x 0", "close the focused pane", "pane"],
   ["Delete other windows", "pane-only", "C-x 1", "keep only the focused pane", "pane"],
   ["Split below", "pane-split-below", "C-x 2", "split the pane horizontally", "pane"],
   ["Split right", "pane-split-right", "C-x 3", "split the pane vertically", "pane"],
   ["Mark whole buffer", "mark-whole", "C-x h", "select the whole buffer", "edit"],
   ["Set mark", "set-mark", "C-SPC", "start a region at point, C-g clears it", "edit"],
   ["Set mark", "set-mark", "C-@", "start a region at point, C-g clears it", "edit"],
   ["Exchange point and mark", "exchange-mark", "C-x C-x", "swap point and mark, reactivating the region", "edit"],
   ["Undo", "undo", "C-x u", "undo the last change", "edit"],
   ["Quit", "quit", "C-x C-c", "ask before closing", "ui"],
   ["Incremental search", "find", "C-s", "search forward", "search"],
   ["Search backward", "find-prev", "C-r", "previous match", "search"],
   ["Query replace", "replace", "M-%", "replace with confirmation", "search"],
   ["Cut region", "cut", "C-w", "kill the selection", "edit"],
   ["Copy region", "copy", "M-w", "copy the selection", "edit"],
   ["Yank", "paste", "C-y", "paste the last kill", "edit"],
   ["Kill line", "kill-line", "C-k", "kill to the end of the line", "edit"],
   ["Forward char", "move-right", "C-f", "move one character right", "cursor"],
   ["Backward char", "move-left", "C-b", "move one character left", "cursor"],
   ["Next line", "move-down", "C-n", "move one line down", "cursor"],
   ["Previous line", "move-up", "C-p", "move one line up", "cursor"],
   ["Forward word", "move-right-fast", "M-f", "move one word right", "cursor"],
   ["Backward word", "move-left-fast", "M-b", "move one word left", "cursor"],
   ["Line start", "line-start", "C-a", "move to the start of the line", "cursor"],
   ["Line end", "line-end", "C-e", "move to the end of the line", "cursor"],
   ["Buffer start", "buffer-start", "M-<", "move to the start of the buffer", "cursor"],
   ["Buffer end", "buffer-end", "M->", "move to the end of the buffer", "cursor"],
   ["Scroll down", "page-down", "C-v", "next screen", "cursor"],
   ["Scroll up", "page-up", "M-v", "previous screen", "cursor"],
   ["Undo", "undo", "C-/", "undo the last change", "edit"],
   ["Undo", "undo", "C-_", "undo the last change", "edit"],
   ["Redo", "redo", "C-M-_", "redo the last undone change", "edit"],
   ["Execute command", "palette", "M-x", "run any command by name", "ui"],
   ["Keyboard quit", "cancel", "C-g", "cancel the pending chord or prompt", "ui"],
   ["Describe key", "describe-key", "C-h k", "show what the next chord runs", "help"],
   ["Describe bindings", "describe-bindings", "C-h b", "list and search every binding", "help"],
   ["Describe command", "describe-bindings", "C-h x", "search commands by name", "help"],
   ["Describe function", "describe-bindings", "C-h f", "search commands by name", "help"],
   ["Which key", "describe-prefix", "C-h C-h", "show the help prefix bindings", "help"],
]

;; Help and keymap switching exist in every profile.
def _COMMON_COMMANDS = [
   ["Describe key", "describe-key", "C-SPC h k", "show what the next chord runs", "help"],
   ["Describe bindings", "describe-bindings", "C-SPC h b", "list and search every binding", "help"],
   ["Toggle Emacs keymap", "toggle-keymap", "C-SPC t e", "switch between the default and Emacs keymaps", "toggle"],
]

;; Default chords that conflict with Emacs meanings and are not kept in the
;; Emacs profile (C-x/C-h are prefixes there; C-y is yank, C-v scrolls, ...).
def _EMACS_DROP = ["C-x", "C-h", "C-y", "C-v", "C-a", "C-b", "C-f", "C-s", "C-r", "C-l", "C-t", "C-i", "C-j", "C-z", "C-Shift-z", "C-c"]

;; Profile state lives in one dict: module-level `mut` scalars and lists
;; written from these functions read back stale from importing modules.
mut _KM = {"name": "", "built": "", "rows": []}

;; User rebinding: [chord, id] pairs applied on top of the active profile.
;; An empty id unbinds the chord.
mut _USER_BINDS = []
mut _USER_LOADED = false

fn keymap_names() list { ["default", "emacs"] }

fn keymap() str {
   "Returns the active keymap profile: default or emacs(NY_EDITOR_KEYMAP picks the initial one)."
   if to_str(_KM.get("name", "")) == "" {
      def e = env("NY_EDITOR_KEYMAP")
      _KM = _KM.set("name", (e && to_str(e) == "emacs") ? "emacs" : "default")
   }
   to_str(_KM.get("name", "default"))
}

fn set_keymap(str name) bool {
   "Selects a keymap profile; returns false for an unknown name."
   if name != "default" && name != "emacs" { return false }
   _KM = _KM.set("name", name)
   _KM = _KM.set("built", "")
   true
}

fn _chord_dropped(str chord) bool {
   ;; A dropped chord, or a sequence that starts with an Emacs prefix.
   mut i = 0
   while i < _EMACS_DROP.len {
      def d = to_str(_EMACS_DROP.get(i))
      if chord == d || str.startswith(chord, d + " ") { return true }
      i += 1
   }
   mut j = 0
   while j < _EMACS_COMMANDS.len {
      if row_key(_EMACS_COMMANDS.get(j)) == chord { return true }
      j += 1
   }
   false
}

fn _user_chord(str chord) bool {
   mut i = 0
   while i < _USER_BINDS.len {
      if to_str(_USER_BINDS.get(i).get(0, "")) == chord { return true }
      i += 1
   }
   false
}

fn _template_row(str id) list {
   ;; A user binding reuses the name/description/tag of the command it names.
   mut i = 0
   while i < _COMMANDS.len {
      def row = _COMMANDS.get(i)
      if row_id(row) == id { return row }
      i += 1
   }
   mut j = 0
   while j < _EMACS_COMMANDS.len {
      def row = _EMACS_COMMANDS.get(j)
      if row_id(row) == id { return row }
      j += 1
   }
   mut c = 0
   while c < _COMMON_COMMANDS.len {
      def row = _COMMON_COMMANDS.get(c)
      if row_id(row) == id { return row }
      c += 1
   }
   []
}

fn parse_bindings(str text) list {
   "Parses `chord = command-id` lines(# starts a comment; `chord =` unbinds). Returns [chord, id] pairs; malformed lines are skipped."
   mut out = []
   def lines = str.split(text, "\n")
   mut i = 0
   while i < lines.len {
      mut line = str.strip(to_str(lines.get(i)))
      i += 1
      if line.len == 0 || str.startswith(line, "#") { continue }
      def eq = str.find(line, "=")
      if eq <= 0 { continue }
      def chord = str.strip(str.str_slice(line, 0, eq))
      def id = str.strip(str.str_slice(line, eq + 1, line.len))
      if chord.len == 0 { continue }
      out = out.append([chord, id])
   }
   out
}

fn load_bindings_file(str path) int {
   "Loads user bindings from `path`; returns how many were applied, or -1 if the file cannot be read."
   mut text = ""
   mut ok = false
   match file_read(path) {
      ok(v) -> { text = v ok = true }
      err(e) -> { ok = false }
   }
   if !ok { return -1 }
   def pairs = parse_bindings(text)
   mut n = 0
   mut i = 0
   while i < pairs.len {
      def pr = pairs.get(i)
      def id = to_str(pr.get(1, ""))
      if id.len == 0 || _template_row(id).len > 0 {
         rebind(to_str(pr.get(0, "")), id)
         n += 1
      }
      i += 1
   }
   n
}

fn _ensure_user_bindings() {
   if _USER_LOADED { return }
   _USER_LOADED = true
   def path = env("NY_EDITOR_KEYS")
   if path && to_str(path).len > 0 { load_bindings_file(to_str(path)) }
}

fn rebind(str chord, str id) bool {
   "Binds `chord` to command `id` on top of the active keymap(empty `id` unbinds). Returns false for an unknown command."
   if id.len > 0 && _template_row(id).len == 0 { return false }
   mut next = []
   mut i = 0
   while i < _USER_BINDS.len {
      def pr = _USER_BINDS.get(i)
      if to_str(pr.get(0, "")) != chord { next = next.append(pr) }
      i += 1
   }
   _USER_BINDS = next.append([chord, id])
   _KM = _KM.set("built", "")
   true
}

fn unbind(str chord) bool { rebind(chord, "") }

fn user_bindings() list { _USER_BINDS }

fn _profile_chord(str profile, str chord) str {
   ;; Emacs keeps C-SPC for the mark; the leader moves to C-c, the prefix
   ;; Emacs reserves for user and mode bindings.
   if profile == "emacs" && str.startswith(chord, "C-SPC ") { return "C-c " + str.str_slice(chord, 6, chord.len) }
   chord
}

fn _build_rows() list {
   _ensure_user_bindings()
   mut out = []
   ;; User bindings first, so they win over the profile's rows.
   mut u = 0
   while u < _USER_BINDS.len {
      def pr = _USER_BINDS.get(u)
      def id = to_str(pr.get(1, ""))
      if id.len > 0 {
         def t = _template_row(id)
         out = out.append([to_str(t.get(0, id)), id, to_str(pr.get(0, "")), to_str(t.get(3, "")), to_str(t.get(4, "user"))])
      }
      u += 1
   }
   def k = keymap()
   if k == "emacs" {
      mut i = 0
      while i < _EMACS_COMMANDS.len {
         def row = _EMACS_COMMANDS.get(i)
         if !_user_chord(row_key(row)) { out = out.append(row) }
         i += 1
      }
   }
   mut j = 0
   while j < _COMMANDS.len {
      def row = _COMMANDS.get(j)
      def chord = row_key(row)
      if k == "emacs" && str.startswith(chord, "C-SPC ") {
         def moved = _profile_chord(k, chord)
         if !_user_chord(moved) { out = out.append([row.get(0, ""), row.get(1, ""), moved, row.get(3, ""), row.get(4, "")]) }
      } elif (k != "emacs" || !_chord_dropped(chord)) && !_user_chord(chord) {
         out = out.append(row)
      }
      j += 1
   }
   mut c = 0
   while c < _COMMON_COMMANDS.len {
      def row = _COMMON_COMMANDS.get(c)
      def chord = _profile_chord(k, row_key(row))
      if !_user_chord(chord) { out = out.append([row.get(0, ""), row.get(1, ""), chord, row.get(3, ""), row.get(4, "")]) }
      c += 1
   }
   out
}

;; Returns the result of the `commands` operation.
fn commands() list { all_commands() }

;; Returns the result of the `all_commands` operation.
fn all_commands() list {
   def k = keymap()
   if to_str(_KM.get("built", "")) != k {
      _KM = _KM.set("rows", _build_rows())
      _KM = _KM.set("built", k)
   }
   _KM.get("rows", [])
}

fn bindings_for(str id) list {
   "Returns every chord bound to command `id` in the active keymap."
   mut out = []
   def rows = all_commands()
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if row_id(row) == id { out = out.append(row_key(row)) }
      i += 1
   }
   out
}

fn describe_chord(str chord) str {
   "Returns a help line for `chord`: the command it runs and its description."
   def rows = all_commands()
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if row_key(row) == chord {
         return chord + " runs " + row_id(row) + " (" + to_str(row.get(0, "")) + "): " + to_str(row.get(3, ""))
      }
      i += 1
   }
   chord + " is not bound"
}

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
   def rows = all_commands()
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
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
   if head == "h" { return "help" }
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
   assert(set_keymap("emacs") && keymap() == "emacs", "emacs keymap selects")
   assert(id_for_chord("C-x C-s") == "save" && id_for_chord("C-y") == "paste" && id_for_chord("C-h k") == "describe-key", "emacs bindings")
   assert(id_for_chord("C-x") == "" && which_key("C-x").len >= 10 && which_key("C-h").len >= 4, "C-x and C-h are prefixes with which-key help")
   assert(describe_chord("C-x C-f").contains("open-file") && describe_chord("C-q").contains("not bound"), "describe-key text")
   assert(set_keymap("default") && id_for_chord("C-x") == "cut" && id_for_chord("C-y") == "redo", "default keymap restored")
   assert(!set_keymap("vim"), "unknown keymap rejected")
}
