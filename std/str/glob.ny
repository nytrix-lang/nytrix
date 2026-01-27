;; Keywords: str glob
;; Simple glob matching.

use std.core *
use std.str *

module std.str.glob (
   glob_match
)

fn _glob_match(p, s, pi, si){
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
   if(pattern == "**/*.ny"){ return endswith(path, ".ny") }
   _glob_match(pattern, path, 0, 0)
}
