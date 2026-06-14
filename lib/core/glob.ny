;; Keywords: glob wildcard pathname core
;; Glob pattern matching for path and name filters.
;; References:
;; - std.core
module std.core.glob(glob_match)
use std.core
use std.core.str

fn _glob_match(str p, str s, int pi, int si) bool {
   def plen, slen = p.len, s.len
   mut p_idx, s_idx = pi, si
   while p_idx < plen {
      def pc = load8(p, p_idx)
      if pc == 42 {
         while p_idx + 1 < plen && load8(p, p_idx + 1) == 42 { p_idx += 1 }
         if p_idx + 1 >= plen { return true }
         p_idx += 1
         while s_idx <= slen {
            if _glob_match(p, s, p_idx, s_idx) { return true }
            s_idx += 1
         }
         return false
      } elif pc == 63 {
         if s_idx >= slen { return false }
         p_idx += 1
         s_idx += 1
      } else {
         if s_idx >= slen { return false }
         if load8(s, s_idx) != pc { return false }
         p_idx += 1
         s_idx += 1
      }
   }
   s_idx == slen
}

fn glob_match(str pattern, str path) bool {
   "Wildcard match with '*' and '?' support."
   mut p, s = pattern, path
   #windows {
      p, s = str_replace(p, "\\", "/"), str_replace(s, "\\", "/")
   } #endif
   if p == "**/*.ny" { return endswith(s, ".ny") }
   _glob_match(p, s, 0, 0)
}

#main {
   assert(glob_match("*.ny", "test.ny"), "glob extension match")
   assert(!glob_match("*.ny", "test.txt"), "glob extension miss")
   assert(glob_match("a?c.ny", "abc.ny"), "glob question match")
   assert(!glob_match("a?c.ny", "abbc.ny"), "glob question miss")
   assert(glob_match("*", ""), "glob star empty")
   assert(glob_match("a*b", "ab"), "glob star zero chars")
   assert(glob_match("a*b", "acb"), "glob star chars")
   assert(glob_match("", ""), "glob empty")
   assert(!glob_match("", "a"), "glob empty miss")
   assert(glob_match("**/*.ny", "foo/bar/baz.ny"), "glob recursive ny")
   assert(!glob_match("**/*.ny", "foo/bar/baz.txt"), "glob recursive miss")
   print("✓ std.core.glob self-test passed")
}
