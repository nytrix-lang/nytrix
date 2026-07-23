;; Printable-string scanner shared by reverse-engineering analysis modules.
module std.os.rev.strings(scan)
use std.core

fn scan(any bytes, int min_len=4, int limit=512) list {
   "Returns printable ASCII string records from bytes with stable offsets."
   mut out = []
   mut i = 0
   while i < bytes.len && out.len < limit {
      mut j = i
      while j < bytes.len {
         def c = load8(bytes, j)
         if c < 32 || c > 126 { break }
         j += 1
      }
      if j - i >= min_len {
         def raw = malloc(max(1, j - i) + 16)
         if raw == 0 { return out }
         def p = raw + 16
         mut k = i
         while k < j {
            store8(p, load8(bytes, k), k - i)
            k += 1
         }
         out = out.append({"offset": i, "len": j - i, "value": init_str(p, j - i)})
      }
      i = max(j + 1, i + 1)
   }
   out
}

#main {
   def found = scan("\x00hello\x00nytrix\x00", 4)
   assert(found.len == 2 && found.get(0).get("value") == "hello" && found.get(1).get("offset") == 7, "reverse string scanner")
   print("✓ std.os.rev.strings self-test passed")
}
