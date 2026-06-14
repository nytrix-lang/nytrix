;; Keywords: window input keyboard mouse key os ui
;; Key normalization and key-chord parsing for std.os.ui input APIs.
;; References:
;; - std.os.ui.window.input
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.input.key(normalize_key, normalize_mod, mod_bit_for_key, mods_from_key_states, parse_notation)
use std.core
use std.core.str
use std.os.ui.window.consts

def _MOD_MASK = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META

comptime table KeyNameCode {
   "ESC", "ESCAPE" -> KEY_ESCAPE
   "RET", "RETURN", "ENTER" -> KEY_ENTER
   "TAB" -> KEY_TAB
   "SPC", "SPACE" -> KEY_SPACE
   "BKSP", "BACKSPACE" -> KEY_BACKSPACE
   "INS", "INSERT" -> KEY_INSERT
   "DEL", "DELETE" -> KEY_DELETE
   "SHIFT", "LSHIFT", "LEFTSHIFT", "LEFT_SHIFT" -> KEY_LEFT_SHIFT
   "RSHIFT", "RIGHTSHIFT", "RIGHT_SHIFT" -> KEY_RIGHT_SHIFT
   "CTRL", "CONTROL", "LCTRL", "LCONTROL", "LEFTCTRL", "LEFT_CONTROL" -> KEY_LEFT_CONTROL
   "RCTRL", "RCONTROL", "RIGHTCTRL", "RIGHT_CONTROL" -> KEY_RIGHT_CONTROL
   "ALT", "OPTION", "LALT", "LOPTION", "LEFTALT", "LEFT_ALT" -> KEY_LEFT_ALT
   "RALT", "ROPTION", "RIGHTALT", "RIGHT_ALT" -> KEY_RIGHT_ALT
   "SUPER", "WIN", "CMD", "COMMAND", "LSUPER", "LCMD", "LEFTSUPER", "LEFT_SUPER" -> KEY_LEFT_SUPER
   "RSUPER", "RCMD", "RIGHTSUPER", "RIGHT_SUPER" -> KEY_RIGHT_SUPER
   "META" -> 0xFFE7
   "MENU", "APPS" -> KEY_MENU
   "UP", "ARROWUP", "ARROW_UP" -> KEY_UP
   "DOWN", "ARROWDOWN", "ARROW_DOWN" -> KEY_DOWN
   "LEFT", "ARROWLEFT", "ARROW_LEFT" -> KEY_LEFT
   "RIGHT", "ARROWRIGHT", "ARROW_RIGHT" -> KEY_RIGHT
   "PGUP", "PAGEUP", "PAGE_UP" -> KEY_PAGE_UP
   "PGDN", "PAGEDN", "PAGEDOWN", "PAGE_DOWN" -> KEY_PAGE_DOWN
   "HOME" -> KEY_HOME
   "END" -> KEY_END
   "CAPS", "CAPSLOCK", "CAPS_LOCK" -> KEY_CAPS_LOCK
   "SCROLLLOCK", "SCROLL_LOCK" -> KEY_SCROLL_LOCK
   "NUMLOCK", "NUM_LOCK" -> KEY_NUM_LOCK
   "PRTSCR", "PRINTSCREEN", "PRINT_SCREEN" -> KEY_PRINT_SCREEN
   "PAUSE", "BREAK" -> KEY_PAUSE
   "GRAVE", "BACKTICK", "`" -> KEY_GRAVE
   "MINUS", "-" -> KEY_MINUS
   "EQUAL", "EQUALS", "=" -> KEY_EQUAL
   "APOSTROPHE", "QUOTE", "'" -> KEY_APOSTROPHE
   "COMMA", "," -> KEY_COMMA
   "PERIOD", "DOT", "." -> KEY_PERIOD
   "SLASH", "/" -> KEY_SLASH
   "SEMICOLON", ";" -> KEY_SEMICOLON
   "LBRACKET", "LEFTBRACKET", "LEFT_BRACKET", "[" -> KEY_LEFT_BRACKET
   "RBRACKET", "RIGHTBRACKET", "RIGHT_BRACKET", "]" -> KEY_RIGHT_BRACKET
   "BACKSLASH", "\\" -> KEY_BACKSLASH
   "F1" -> KEY_F1
   "F2" -> KEY_F2
   "F3" -> KEY_F3
   "F4" -> KEY_F4
   "F5" -> KEY_F5
   "F6" -> KEY_F6
   "F7" -> KEY_F7
   "F8" -> KEY_F8
   "F9" -> KEY_F9
   "F10" -> KEY_F10
   "F11" -> KEY_F11
   "F12" -> KEY_F12
   "F13" -> KEY_F13
   "F14" -> KEY_F14
   "F15" -> KEY_F15
   "F16" -> KEY_F16
   "F17" -> KEY_F17
   "F18" -> KEY_F18
   "F19" -> KEY_F19
   "F20" -> KEY_F20
   "F21" -> KEY_F21
   "F22" -> KEY_F22
   "F23" -> KEY_F23
   "F24" -> KEY_F24
   "F25" -> KEY_F25
   "KP0", "KP_0", "NUM0", "NUM_0", "NUMPAD0", "NUMPAD_0", "KEYPAD0", "KEYPAD_0" -> KEY_KP_0
   "KP1", "KP_1", "NUM1", "NUM_1", "NUMPAD1", "NUMPAD_1", "KEYPAD1", "KEYPAD_1" -> KEY_KP_1
   "KP2", "KP_2", "NUM2", "NUM_2", "NUMPAD2", "NUMPAD_2", "KEYPAD2", "KEYPAD_2" -> KEY_KP_2
   "KP3", "KP_3", "NUM3", "NUM_3", "NUMPAD3", "NUMPAD_3", "KEYPAD3", "KEYPAD_3" -> KEY_KP_3
   "KP4", "KP_4", "NUM4", "NUM_4", "NUMPAD4", "NUMPAD_4", "KEYPAD4", "KEYPAD_4" -> KEY_KP_4
   "KP5", "KP_5", "NUM5", "NUM_5", "NUMPAD5", "NUMPAD_5", "KEYPAD5", "KEYPAD_5" -> KEY_KP_5
   "KP6", "KP_6", "NUM6", "NUM_6", "NUMPAD6", "NUMPAD_6", "KEYPAD6", "KEYPAD_6" -> KEY_KP_6
   "KP7", "KP_7", "NUM7", "NUM_7", "NUMPAD7", "NUMPAD_7", "KEYPAD7", "KEYPAD_7" -> KEY_KP_7
   "KP8", "KP_8", "NUM8", "NUM_8", "NUMPAD8", "NUMPAD_8", "KEYPAD8", "KEYPAD_8" -> KEY_KP_8
   "KP9", "KP_9", "NUM9", "NUM_9", "NUMPAD9", "NUMPAD_9", "KEYPAD9", "KEYPAD_9" -> KEY_KP_9
   "KPDECIMAL", "KP_DECIMAL", "KP_DOT", "NUMPADDECIMAL", "NUMPAD_DECIMAL" -> KEY_KP_DECIMAL
   "KPDIVIDE", "KP_DIVIDE", "KP_SLASH", "NUMPADDIVIDE", "NUMPAD_DIVIDE" -> KEY_KP_DIVIDE
   "KPMULTIPLY", "KP_MULTIPLY", "KP_STAR", "NUMPADMULTIPLY", "NUMPAD_MULTIPLY" -> KEY_KP_MULTIPLY
   "KPSUBTRACT", "KP_SUBTRACT", "KP_MINUS", "NUMPADSUBTRACT", "NUMPAD_SUBTRACT" -> KEY_KP_SUBTRACT
   "KPADD", "KP_ADD", "KP_PLUS", "NUMPADADD", "NUMPAD_ADD" -> KEY_KP_ADD
   "KPENTER", "KP_ENTER", "NUMPADENTER", "NUMPAD_ENTER" -> KEY_KP_ENTER
   "KPEQUAL", "KP_EQUAL", "KP_EQUALS", "NUMPADEQUAL", "NUMPAD_EQUAL" -> KEY_KP_EQUAL
}

comptime table KeyPrefixMod {
   "CONTROL", "CTRL", "C" -> MOD_CONTROL
   "SHIFT", "S" -> MOD_SHIFT
   "OPTION", "ALT", "A", "M" -> MOD_ALT
   "META", "G" -> MOD_META
   "COMMAND", "CMD", "WIN", "SUPER", "HYPER", "H", "D" -> MOD_SUPER
}

fn normalize_mod(any mod) i32 {
   "Masks a modifier bitset down to the supported std.os.ui modifier flags."
   if is_str(mod) { return _prefix_mod(upper(strip(to_str(mod)))) & _MOD_MASK }
   if is_int(mod) || is_float(mod) { return int(mod) & _MOD_MASK }
   0
}

fn _normalize_named_key(str name) i32 {
   def n = upper(strip(name))
   if n.len == 0 { return KEY_NULL }
   if n.len == 1 {
      def c = load8(n, 0)
      if c >= 97 && c <= 122 { return c - 32 }
      return c
   }
   comptime match KeyNameCode(n, KEY_NULL)
}

fn normalize_key(any key) i32 {
   "Normalizes native key codes for stable comparisons across backends."
   if is_str(key) { return _normalize_named_key(to_str(key)) }
   if !is_int(key) && !is_float(key) { return KEY_NULL }
   def k = int(key)
   case k {
      97..122 -> k - 32
      16 -> KEY_LEFT_SHIFT
      17 -> KEY_LEFT_CONTROL
      18 -> KEY_LEFT_ALT
      13, 0xFF0D, KEY_ENTER -> KEY_ENTER
      8, 0xFF08, KEY_BACKSPACE -> KEY_BACKSPACE
      9, 0xFF09, KEY_TAB -> KEY_TAB
      KEY_INSERT, 0xFF63 -> KEY_INSERT
      127, 0xFFFF, KEY_DELETE -> KEY_DELETE
      27, 0xFF1B, KEY_ESCAPE -> KEY_ESCAPE
      1000, 0xFF51, KEY_LEFT -> KEY_LEFT
      1001, 0xFF52, KEY_UP -> KEY_UP
      1002, 0xFF53, KEY_RIGHT -> KEY_RIGHT
      1003, 0xFF54, KEY_DOWN -> KEY_DOWN
      1004, 0xFF55, KEY_PAGE_UP -> KEY_PAGE_UP
      1005, 0xFF56, KEY_PAGE_DOWN -> KEY_PAGE_DOWN
      1006, 0xFF50, KEY_HOME -> KEY_HOME
      1007, 0xFF57, KEY_END -> KEY_END
      1008..1032 -> KEY_F1 + (k - 1008)
      0xFFBE..0xFFD6 -> KEY_F1 + (k - 0xFFBE)
      _ -> k
   }
}

fn mod_bit_for_key(any key) i32 {
   "Returns the modifier bit corresponding to the given native key code."
   def k = normalize_key(key)
   case k {
      0xFFE1, 0xFFE2, 16, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT -> MOD_SHIFT
      0xFFE3, 0xFFE4, 17, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL -> MOD_CONTROL
      0xFFE9, 0xFFEA, 18, KEY_LEFT_ALT, KEY_RIGHT_ALT -> MOD_ALT
      0xFFEB, 0xFFEC, KEY_LEFT_SUPER, KEY_RIGHT_SUPER -> MOD_SUPER
      0xFFE7, 0xFFE8 -> MOD_META
      _ -> 0
   }
}

fn mods_from_key_states(any ks) i32 {
   "Reconstructs the active modifier bitset from a key-state dictionary."
   if !is_dict(ks) { return 0 }
   mut mods = 0
   def items = dict_items(ks)
   mut i = 0
   def n = items.len
   while i < n {
      def kv = items.get(i, 0)
      if is_list(kv) && kv.len >= 2 && kv.get(1, false) {
         def key = normalize_key(kv.get(0, 0))
         mods = mods | mod_bit_for_key(key)
      }
      i += 1
   }
   normalize_mod(mods)
}

fn _prefix_mod(str seg) i32 { comptime match KeyPrefixMod(seg, 0) }

fn _parse_single_key(str tok) list {
   mut mods = 0
   def parts = split(upper(tok), "-")
   def n = parts.len
   if n <= 0 { return [0, mods] }
   mut i = 0
   while i + 1 < n {
      mods = mods | _prefix_mod(parts.get(i, ""))
      i += 1
   }
   def p = parts.get(n - 1, "")
   def key = normalize_key(p)
   [key, mods]
}

fn parse_notation(str notation) list {
   "Parses a key-sequence notation string into a list of `[key, mod]` pairs."
   def toks = split(notation, " ")
   mut seq = []
   mut i = 0
   def toks_n = toks.len
   while i < toks_n {
      def tok = toks.get(i)
      if tok.len > 0 { seq = seq.append(_parse_single_key(tok)) }
      i += 1
   }
   seq
}

#main {
   assert(normalize_key(0xFFBE) == KEY_F1 && normalize_key(0xFFC9) == KEY_F12 && normalize_key(1008) == KEY_F1 && normalize_key(1032) == KEY_F25, "input numeric keys")
   assert(normalize_key("F1") == KEY_F1 && normalize_key("f13") == KEY_F13 && normalize_key("Escape") == KEY_ESCAPE && normalize_key("Page_Down") == KEY_PAGE_DOWN, "input named keys")
   assert(normalize_key("KP_0") == KEY_KP_0 && normalize_key("NumpadEnter") == KEY_KP_ENTER && mod_bit_for_key("shift") == MOD_SHIFT && mod_bit_for_key("right_control") == MOD_CONTROL, "input keypad/mod keys")
   def f1 = parse_notation("F1").get(0)
   def f12 = parse_notation("F12").get(0)
   def f13 = parse_notation("F13").get(0)
   def kp0 = parse_notation("KP_0").get(0)
   def c_f13 = parse_notation("C-F13").get(0)
   assert(f1.get(0) == KEY_F1 && f12.get(0) == KEY_F12 && f13.get(0) == KEY_F13 && kp0.get(0) == KEY_KP_0, "input notation basics")
   assert(c_f13.get(0) == KEY_F13 && (c_f13.get(1) & MOD_CONTROL) != 0, "input notation modifiers")
   assert(normalize_key("[") == KEY_LEFT_BRACKET && normalize_key("\\") == KEY_BACKSLASH && mod_bit_for_key("[") == 0 && mod_bit_for_key("\\") == 0, "input bracket/backslash are not super")
   mut ks = dict(4)
   ks = ks.set(KEY_LEFT_SHIFT, true)
   ks = ks.set(KEY_RIGHT_CONTROL, true)
   ks = ks.set(KEY_LEFT_ALT, false)
   ks = ks.set("right_alt", true)
   assert(mods_from_key_states(ks) == (MOD_SHIFT | MOD_CONTROL | MOD_ALT), "input key state mods")
   print("✓ std.os.ui.window.input.key self-test passed")
}
