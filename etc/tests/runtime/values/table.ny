use std.core
use std.core.error

def KEY_NULL = 0
def KEY_ESCAPE = 256
def KEY_HOME = 257
def KEY_LEFT = 258
def KEY_F1 = 1000

comptime table KeyMap {
   0x30..0x39 -> raw
   0x41..0x5a -> raw
   0x61..0x7a -> raw - 32
   0xff1b -> KEY_ESCAPE
   0xff50 -> KEY_HOME
   0xff51 -> KEY_LEFT
   0xffbe..0xffc9 -> KEY_F1 + (raw - 0xffbe)
}

fn map_key(i32 raw) i32 = comptime match KeyMap(raw, KEY_NULL)
assert(map_key(0x35) == 0x35, "comptime table digit range")
assert(map_key(0x61) == 0x41, "comptime table lowercase fold")
assert(map_key(0xff1b) == KEY_ESCAPE, "comptime table literal")
assert(map_key(0xffc1) == KEY_F1 + 3, "comptime table function range")
assert(map_key(0) == KEY_NULL, "comptime table fallback")
assert(_key_map(0xff50) == KEY_HOME, "comptime table legacy helper literal")
assert(_key_map(0, -123) == -123, "comptime table legacy helper explicit fallback")

comptime table SemanticKind {
   1, 2, 3 -> 10
   10..19 -> raw * 2
   _ -> 99
}

fn semantic_kind(i32 raw) i32 = comptime match SemanticKind(raw, -1)
assert(semantic_kind(2) == 10, "comptime table multi-pattern")
assert(semantic_kind(12) == 24, "comptime table range expression")
assert(semantic_kind(40) == 99, "comptime table wildcard")
assert(_semantic_kind(40) == 99, "comptime table legacy helper wildcard")

module TableModule(
   map_mod_key
){
   comptime table ModKeyMap {
      7 -> raw + 1
      _ -> default
   }
   fn map_mod_key(i32 raw) i32 = comptime match ModKeyMap(raw, -7)
}

use TableModule (map_mod_key)

assert(map_mod_key(7) == 8, "module-local comptime table")
assert(map_mod_key(99) == -7, "module-local comptime table fallback")
print("✓ comptime table tests passed")
