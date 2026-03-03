;; Keywords: str glob
;; Simple glob matching.

module std.str.glob (
   glob_match
)
use std.core *
use std.str *
use std.os *

fn _glob_match(p, s, pi, si){
   "Internal recursive matcher used by `glob_match`."
   def plen = str_len(p)
   def slen = str_len(s)
   mut p_idx = pi
   mut s_idx = si
   while(p_idx < plen){
      def pc = load8(p, p_idx)
      if(pc == 42){ ; '*'
         while(p_idx + 1 < plen && load8(p, p_idx + 1) == 42){ p_idx += 1 }
         if(p_idx + 1 >= plen){ return 1 }
         p_idx += 1
         while(s_idx <= slen){
         if(_glob_match(p, s, p_idx, s_idx)){ return 1 }
         s_idx += 1
         }
         return 0
      } elif(pc == 63){ ; '?'
         if(s_idx >= slen){ return 0 }
         p_idx += 1
         s_idx += 1
      } else {
         if(s_idx >= slen){ return 0 }
         if(load8(s, s_idx) != pc){ return 0 }
         p_idx += 1
         s_idx += 1
      }
   }
   return s_idx == slen
}

fn glob_match(pattern, path){
   "Wildcard match with '*' and '?' support."
   mut p = pattern
   mut s = path
   if(__os_name() == "windows"){
      p = replace_all(p, "\\", "/")
      s = replace_all(s, "\\", "/")
   }
   if(p == "**/*.ny"){ return endswith(s, ".ny") }
   _glob_match(p, s, 0, 0)
}

if(comptime{__main()}){
   use std.core *
   use std.str.glob *

   assert(glob_match("*.ny", "test.ny"), "glob *.ny match")
   assert(!glob_match("*.ny", "test.txt"), "glob *.ny non-match")
   assert(glob_match("a?c.ny", "abc.ny"), "glob ? match")
   assert(!glob_match("a?c.ny", "abbc.ny"), "glob ? non-match")
   assert(glob_match("**/test/*.ny", "std/core/test/mod.ny"), "glob ** match")

   ; Basic wildcard '*' tests
   assert(glob_match("*", "anything"), "glob * matches anything")
   assert(glob_match("*", ""), "glob * matches empty string")
   assert(glob_match("a*b", "ab"), "glob a*b matches ab")
   assert(glob_match("a*b", "acb"), "glob a*b matches acb")
   assert(glob_match("a*b", "a/b"), "glob a*b matches a/b")
   assert(glob_match("*a*", "bab"), "glob *a* matches bab")
   assert(!glob_match("*a*", "bb"), "glob *a* non-match bb")

   ; Basic wildcard '?' tests
   assert(glob_match("?", "a"), "glob ? matches single char")
   assert(!glob_match("?", ""), "glob ? non-match empty")
   assert(glob_match("a?b", "acb"), "glob a?b matches acb")
   assert(glob_match("a?b", "a/b"), "glob a?b matches a/b")

   ; Empty pattern tests
   assert(glob_match("", ""), "glob empty matches empty")
   assert(!glob_match("", "a"), "glob empty non-match a")

   ; '**' behavior (acts as '*')
   assert(glob_match("a**b", "ab"), "glob a**b matches ab")
   assert(glob_match("a**b", "acb"), "glob a**b matches acb")

   ; Specific **/*.ny optimization
   assert(glob_match("**/*.ny", "foo/bar/baz.ny"), "glob **/*.ny match nested")
   assert(!glob_match("**/*.ny", "foo/bar/baz.txt"), "glob **/*.ny non-match ext")

   print("✓ std.str.glob tests passed")
}
