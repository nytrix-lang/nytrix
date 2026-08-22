module std.core.format
use std.core

; Pure-Ny string formatting with padding, alignment, and number formatting.
fn pad(str s, int width, str ch, bool left_align) str {
   "Pad string `s` to at least `width` characters using `ch`."
   def n = len(s)
   if n >= width { return s }
   def pad_count = width - n
   mut fill = ""
   mut i = 0
   while i < pad_count {
      fill = fill + ch
      i = i + 1
   }
   if left_align { return s + fill }
   return fill + s
}

fn lpad(str s, int width, str ch) str {
   "Left-align: pad on the right."
   return pad(s, width, ch, true)
}

fn rpad(str s, int width, str ch) str {
   "Right-align: pad on the left."
   return pad(s, width, ch, false)
}

fn center(str s, int width, str ch) str {
   "Center-align string `s` within `width`."
   def n = len(s)
   if n >= width { return s }
   def total_pad = width - n
   def left = total_pad / 2
   def right = total_pad - left
   mut lfill = ""
   mut i = 0
   while i < left {
      lfill = lfill + ch
      i = i + 1
   }
   mut rfill = ""
   i = 0
   while i < right {
      rfill = rfill + ch
      i = i + 1
   }
   return lfill + s + rfill
}

fn fmt_int(int v, int base, bool upper) str {
   "Format an integer in the given base(2-36)."
   def digits_lower = "0123456789abcdefghijklmnopqrstuvwxyz"
   def digits_upper = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
   def digits = upper ? digits_upper : digits_lower
   if v == 0 { return "0" }
   mut result = ""
   mut n = v
   mut negative = false
   if n < 0 {
      negative = true
      n = -n
   }
   while n > 0 {
      def idx = n % base
      def ch = digits.get(idx)
      result = ch + result
      n = n / base
   }
   if negative { result = "-" + result }
   return result
}

fn fmt_hex(int v) str {
   "Format integer as hexadecimal with 0x prefix."
   if v == 0 { return "0x0" }
   return "0x" + fmt_int(v, 16, false)
}

fn fmt_oct(int v) str {
   "Format integer as octal with 0o prefix."
   if v == 0 { return "0o0" }
   return "0o" + fmt_int(v, 8, false)
}

fn fmt_bin(int v) str {
   "Format integer as binary with 0b prefix."
   if v == 0 { return "0b0" }
   return "0b" + fmt_int(v, 2, false)
}

fn repeat_str(str s, int n) str {
   "Repeat string `s` `n` times."
   if n <= 0 { return "" }
   mut result = ""
   mut i = 0
   while i < n {
      result = result + s
      i = i + 1
   }
   return result
}

fn join_items(sep, seq items) str {
   "Join sequence items with separator."
   def n = len(items)
   if n == 0 { return "" }
   mut result = to_str(items.get(0))
   mut i = 1
   while i < n {
      result = result + sep + to_str(items.get(i))
      i = i + 1
   }
   return result
}

fn hexdump(bytes data, int start, int count) str {
   "Produce a hex dump of `data` starting at `start` for `count` bytes."
   mut result = ""
   mut i = start
   def end = start + count
   while i < end {
      mut hex_bytes = ""
      mut ascii_str = ""
      mut j = 0
      while j < 16 && (i + j) < end {
         def b = data[i + j]
         def h = fmt_int(b, 16, false)
         hex_bytes = hex_bytes + rpad(h, 2, "0") + " "
         if b >= 32 && b <= 126 {
            ascii_str = ascii_str + chr(b)
         } else {
            ascii_str = ascii_str + "."
         }
         j = j + 1
      }
      result = result + rpad(fmt_hex(i), 10, " ") + "  " + rpad(hex_bytes, 49, " ") + "|" + ascii_str + "|"
      if (i + 16) < end { result = result + "\n" }
      i = i + 16
   }
   return result
}

#main {
   assert_eq(rpad("hi", 5, "."), "...hi", "rpad")
   assert_eq(lpad("hi", 5, "."), "hi...", "lpad")
   assert_eq(center("hi", 6, "."), "..hi..", "center")
   assert_eq(fmt_int(255, 16, false), "ff", "fmt_int hex")
   assert_eq(fmt_hex(255), "0xff", "fmt_hex")
   assert_eq(fmt_bin(5), "0b101", "fmt_bin")
   assert_eq(fmt_oct(8), "0o10", "fmt_oct")
   assert_eq(repeat_str("ab", 3), "ababab", "repeat_str")
   assert_eq(join_items(", ", [1, 2, 3]), "1, 2, 3", "join_items")
   print("format tests passed")
}
