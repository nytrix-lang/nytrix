;; Keywords: bytes binary endian strings elf
;; Bounds-checked binary byte and string primitives for ELF readers.
module std.os.rev.decomp.bytes(_u16, _u32, _u64, _slice, _slice_list, _cstring)
use std.core
use std.core.str as str

fn _u16(str b, int off, bool le=true) int {
   if off < 0 || off + 1 >= b.len { return 0 }
   def a = load8(b, off)
   def c = load8(b, off + 1)
   le ? (a | (c << 8)) : ((a << 8) | c)
}

fn _u32(str b, int off, bool le=true) int {
   if off < 0 || off + 3 >= b.len { return 0 }
   if le {
      return load8(b, off) | (load8(b, off + 1) << 8) |
      (load8(b, off + 2) << 16) | (load8(b, off + 3) << 24)
   }
   (load8(b, off) << 24) | (load8(b, off + 1) << 16) |
   (load8(b, off + 2) << 8) | load8(b, off + 3)
}

fn _u64(str b, int off, bool le=true) int {
   if off < 0 || off + 7 >= b.len { return 0 }
   le ? (_u32(b, off, true) | (_u32(b, off + 4, true) << 32)) :
   ((_u32(b, off, false) << 32) | _u32(b, off + 4, false))
}

fn _slice(str b, int off, int n) str {
   if off < 0 || n <= 0 || off >= b.len { return "" }
   def end = min(b.len, off + n)
   def base = malloc(max(1, end - off) + 16)
   if base == 0 { return "" }
   def p = base + 16
   mut i = off
   while i < end { store8(p, load8(b, i), i - off) i += 1 }
   init_str(p, end - off)
}

fn _slice_list(str b, int off, int n) list {
   if off < 0 || n <= 0 || off >= b.len { return [] }
   def end = min(b.len, off + n)
   mut out = []
   mut i = off
   while i < end { out = out.append(load8(b, i)) i += 1 }
   out
}

fn _cstring(str b, int off, int maxn=4096) str {
   if off < 0 || off >= b.len { return "" }
   mut out = str.Builder(32)
   mut i = off
   mut left = maxn
   while i < b.len && left > 0 {
      def c = load8(b, i)
      if c == 0 { break }
      out = str.builder_append(out, chr(c))
      i += 1
      left -= 1
   }
   def s = str.builder_to_str(out)
   str.builder_free(out)
   s
}

#main {
   assert(_u16("\x34\x12", 0) == 0x1234 && _u32("\x78\x56\x34\x12", 0) == 0x12345678, "little-endian reads")
   assert(_slice("abcdef", 2, 3) == "cde" && _cstring("ok\x00tail", 0) == "ok", "bounded byte strings")
   print("✓ std.os.rev.decomp.bytes self-test passed")
}
