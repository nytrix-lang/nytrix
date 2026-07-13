;; Keywords: editor interaction cursor mouse selection os ui render viewer text
;; Mouse and keyboard interaction handling for editor cursors and selections.
;; References:
;; - std.os.ui.render.viewer.editor.selection
;; - std.os.ui.render.viewer.input
module std.os.ui.render.viewer.editor.interaction(
   new, message, tick, command, key_seen, show_key, status,
   history, history_add, last_command, clear_key
)

use std.core
use std.core.str as str

;; Creates a new module state with the supplied configuration.
fn new() dict {
   {"message": "ready", "timer": 0.0, "show_key": "", "key_timer": 0.0, "history": [], "last": ""}
}

;; Returns the result of the `message` operation.
fn message(dict st, str text, f64 ttl=1.5) dict {
   st["message"] = text
   st["timer"] = ttl
   st
}

;; Returns the result of the `history_add` operation.
fn history_add(dict st, str id) dict {
   if id.len <= 0 { return st }
   mut h = st.get("history", [])
   if h.len == 0 || to_str(h.get(h.len - 1, "")) != id { h = h.append(id) }
   if h.len > 64 { h = slice(h, h.len - 64, h.len, 1) }
   st["history"] = h
   st["last"] = id
   st
}

;; Returns the result of the `command` operation.
fn command(dict st, str id, str text="") dict {
   st = history_add(st, id)
   message(st, text.len > 0 ? text : id, 1.5)
}

;; Returns the result of the `key_seen` operation.
fn key_seen(dict st, str chord) dict {
   if chord.len <= 0 { return st }
   st["show_key"] = chord
   st["key_timer"] = 1.2
   st
}

;; Releases the key.
fn clear_key(dict st) dict {
   st["show_key"] = ""
   st["key_timer"] = 0.0
   st
}

;; Returns the result of the `tick` operation.
fn tick(dict st, f64 dt) dict {
   if float(st.get("timer", 0.0)) > 0.0 { st["timer"] = max(0.0, float(st.get("timer", 0.0)) - dt) }
   if float(st.get("key_timer", 0.0)) > 0.0 {
      st["key_timer"] = max(0.0, float(st.get("key_timer", 0.0)) - dt)
      if float(st.get("key_timer", 0.0)) <= 0.0 { st["show_key"] = "" }
   }
   st
}

fn show_key(dict st) str { to_str(st.get("show_key", "")) }

;; Returns the result of the `status` operation.
fn status(dict st) str { float(st.get("timer", 0.0)) > 0.0 ? to_str(st.get("message", "ready")) : "ready" }

fn history(dict st) list { st.get("history", []) }

fn last_command(dict st) str { to_str(st.get("last", "")) }

#main {
   mut st = new()
   st = key_seen(command(st, "save"), "C-SPC f s")
   st = tick(st, 2.0)
   assert(last_command(st) == "save" && show_key(st) == "", "editor interaction")
}
