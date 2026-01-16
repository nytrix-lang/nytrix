;; Keywords: util regex
;; Util Regex module.

use std.core
module std.util.regex (
   _re_match_here, _re_match_star, regex_match, regex_find
)

fn _re_match_here(p, pi, s, si){
   "Internal: match pattern starting at pi against string at si."
   def pc = __load8_idx(p, pi)
   if(pc == 0){ return 1 }
   def next_idx = pi + 1
   def next_p = __load8_idx(p, next_idx)
   if(next_p == 42){
      return _re_match_star(pc, p, pi + 2, s, si)
   }
   if(pc == 36 && next_p == 0){
      if(__load8_idx(s, si) == 0){ return 1 }
      return 0
   }
   def sc = __load8_idx(s, si)
   if(sc != 0){
      if(pc == 46 || pc == sc){
         return _re_match_here(p, pi + 1, s, si + 1)
      }
   }
   return 0
}

fn _re_match_star(c, p, pi, s, si){
   "Internal: handle '*' repetition for regex matcher."
   def i = si
   while(1){
      if(_re_match_here(p, pi, s, i) == 1){ return 1 }
      def sc = __load8_idx(s, i)
      if(sc == 0){ return 0 }
      if(c != 46 && sc != c){ return 0 }
      i = i + 1
   }
   return 0
}

fn regex_match(pat, s){
   "Return 1 if pattern matches string (supports . * ^ $)."
   if(__load8_idx(pat, 0) == 94){
      return _re_match_here(pat, 1, s, 0)
   }
   def i = 0
   while(1){
      if(_re_match_here(pat, 0, s, i) == 1){ return 1 }
      if(__load8_idx(s, i) == 0){ break }
      i = i + 1
   }
   return 0
}

fn regex_find(pat, s){
   "Return index of first match or -1 (supports . * ^ $)."
   if(__load8_idx(pat, 0) == 94){
      if(_re_match_here(pat, 1, s, 0) == 1){ return 0 }
      return -1
   }
   def i = 0
   while(1){
      if(_re_match_here(pat, 0, s, i) == 1){ return i }
      if(__load8_idx(s, i) == 0){ break }
      i = i + 1
   }
   return -1
}