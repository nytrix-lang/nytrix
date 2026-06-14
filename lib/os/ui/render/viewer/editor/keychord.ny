;; Keywords: editor keychord shortcuts input os ui render viewer text
;; Editor key chord parsing and shortcut matching helpers.
;; References:
;; - std.os.ui.render.viewer.keyboard
module std.os.ui.render.viewer.editor.keychord(
   PREFIX_TIMEOUT, empty_state, tick, clear, pending, describe, event_chord, command_chord
)

use std.core
use std.core.str as str
use std.os.ui.render.viewer.editor.commands as commands
use std.os.ui.window.consts as key

def PREFIX_TIMEOUT = 2.0

fn empty_state() dict {
   {"prefix": "", "timer": 0.0}
}

fn tick(dict st, f64 dt) dict {
   if to_str(st.get("prefix", "")).len > 0 {
      st["timer"] = float(st.get("timer", 0.0)) - dt
      if float(st.get("timer", 0.0)) <= 0.0 { st = clear(st) }
   }
   st
}

fn clear(dict st) dict {
   st["prefix"] = ""
   st["timer"] = 0.0
   st
}

fn pending(dict st) bool {
   to_str(st.get("prefix", "")).len > 0
}

fn describe(dict st) str {
   to_str(st.get("prefix", ""))
}

fn _mods(any data) int {
   int(data.get("mods", data.get("mod", 0)))
}

fn _key_name(int k) str {
   case k {
      key.KEY_A..key.KEY_Z -> chr(97 + (k - key.KEY_A))
      key.KEY_0..key.KEY_9 -> chr(48 + (k - key.KEY_0))
      key.KEY_TAB -> "TAB"
      key.KEY_ENTER -> "RET"
      key.KEY_ESCAPE -> "ESC"
      key.KEY_BACKSPACE -> "BACKSPACE"
      key.KEY_DELETE -> "DELETE"
      key.KEY_SPACE -> "SPC"
      key.KEY_LEFT -> "LEFT"
      key.KEY_RIGHT -> "RIGHT"
      key.KEY_UP -> "UP"
      key.KEY_DOWN -> "DOWN"
      key.KEY_HOME -> "HOME"
      key.KEY_END -> "END"
      key.KEY_PAGE_UP -> "PGUP"
      key.KEY_PAGE_DOWN -> "PGDN"
      key.KEY_KP_0..key.KEY_KP_9 -> "KP" + to_str(k - key.KEY_KP_0)
      key.KEY_KP_ADD -> "+"
      key.KEY_KP_SUBTRACT -> "-"
      key.KEY_KP_MULTIPLY -> "*"
      key.KEY_KP_DIVIDE -> "/"
      key.KEY_KP_DECIMAL -> "."
      key.KEY_KP_ENTER -> "RET"
      key.KEY_KP_EQUAL -> "="
      key.KEY_MINUS -> "-"
      key.KEY_EQUAL -> "="
      key.KEY_SEMICOLON -> ";"
      key.KEY_APOSTROPHE -> "'"
      key.KEY_COMMA -> ","
      key.KEY_PERIOD -> "."
      key.KEY_SLASH -> "/"
      key.KEY_GRAVE -> "`"
      key.KEY_LEFT_BRACKET -> "["
      key.KEY_RIGHT_BRACKET -> "]"
      key.KEY_BACKSLASH -> "\\"
      key.KEY_F1..key.KEY_F25 -> "F" + to_str(k - key.KEY_F1 + 1)
      _ -> "KEY" + to_str(k)
   }
}

fn _shifted_printable_name(int k) str {
   case k {
      key.KEY_0 -> ")"
      key.KEY_1 -> "!"
      key.KEY_2 -> "@"
      key.KEY_3 -> "#"
      key.KEY_4 -> "$"
      key.KEY_5 -> "%"
      key.KEY_6 -> "^"
      key.KEY_7 -> "&"
      key.KEY_8 -> "*"
      key.KEY_9 -> "("
      key.KEY_MINUS -> "_"
      key.KEY_EQUAL -> "+"
      key.KEY_SEMICOLON -> ":"
      key.KEY_APOSTROPHE -> "\""
      key.KEY_COMMA -> "<"
      key.KEY_PERIOD -> ">"
      key.KEY_SLASH -> "?"
      key.KEY_GRAVE -> "~"
      key.KEY_LEFT_BRACKET -> "{"
      key.KEY_RIGHT_BRACKET -> "}"
      key.KEY_BACKSLASH -> "|"
      _ -> ""
   }
}

fn event_chord(any data) str {
   "Returns a normalized chord like C-x, M-x, C-Shift-p, C-+."
   def mods = _mods(data)
   def k = int(data.get("key", key.KEY_NULL))
   def shifted = (mods & key.MOD_SHIFT) != 0
   def shifted_name = shifted ? _shifted_printable_name(k) : ""
   mut out = ""
   if (mods & key.MOD_CONTROL) != 0 { out += "C-" }
   if (mods & (key.MOD_ALT)) != 0 { out += "M-" }
   if (mods & (key.MOD_SUPER | key.MOD_META)) != 0 { out += "S-" }
   if shifted && shifted_name.len <= 0 { out += "Shift-" }
   out + (shifted_name.len > 0 ? shifted_name : _key_name(k))
}

fn _is_prefix(str chord) bool {
   def p = chord + " "
   def rows = commands.all_commands()
   mut i = 0
   while i < rows.len {
      if str.startswith(commands.row_key(rows.get(i)), p) { return true }
      i += 1
   }
   false
}

fn command_chord(dict st, any data) list {
   "Returns [next_state, command] where command can be a complete chord or empty while waiting for a suffix."
   def ch = event_chord(data)
   def prefix = to_str(st.get("prefix", ""))
   if ch == "C-g" {
      return [clear(st), "cancel"]
   }
   def full = prefix.len > 0 ? prefix + " " + ch : ch
   if _is_prefix(full) {
      st["prefix"] = full
      st["timer"] = PREFIX_TIMEOUT
      return [st, ""]
   }
   [prefix.len > 0 ? clear(st) : st, full]
}

#main {
   mut st = empty_state()
   def a = command_chord(st, {"key": key.KEY_SPACE, "mods": key.MOD_CONTROL})
   st = a.get(0)
   assert(pending(st) && to_str(a.get(1, "")) == "", "keychord prefix")
   def b1 = command_chord(st, {"key": key.KEY_F, "mods": 0})
   st = b1.get(0)
   def b = command_chord(st, {"key": key.KEY_S, "mods": 0})
   assert(to_str(b.get(1, "")) == "C-SPC f s", "keychord sequence")
   def c = command_chord(empty_state(), {"key": key.KEY_SPACE, "mods": key.MOD_CONTROL})
   def d = command_chord(c.get(0), {"key": key.KEY_L, "mods": 0})
   def e = command_chord(d.get(0), {"key": key.KEY_S, "mods": 0})
   assert(to_str(e.get(1, "")) == "C-SPC l s", "keychord long sequence")
   assert(event_chord({"key": key.KEY_EQUAL, "mods": key.MOD_SHIFT | key.MOD_CONTROL}) == "C-+", "keychord shifted punctuation")
   assert(event_chord({"key": key.KEY_KP_ADD, "mods": key.MOD_CONTROL}) == "C-+", "keychord keypad add")
   assert(event_chord({"key": key.KEY_P, "mods": key.MOD_SHIFT | key.MOD_CONTROL}) == "C-Shift-p", "keychord shifted letter")
   def r0 = command_chord(empty_state(), {"key": key.KEY_C, "mods": key.MOD_CONTROL})
   assert(pending(r0.get(0)) && to_str(r0.get(1, "")) == "", "keychord ctrl-c prefix")
   def r1 = command_chord(r0.get(0), {"key": key.KEY_R, "mods": 0})
   def r2 = command_chord(r1.get(0), {"key": key.KEY_R, "mods": 0})
   assert(to_str(r2.get(1, "")) == "C-c r r", "keychord ctrl-c run")
   print("✓ viewer editor keychord test passed")
}
