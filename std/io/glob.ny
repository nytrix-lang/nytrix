;; Keywords: io glob
;; Io Glob module.

module std.io.glob (
   _glob_match_here, glob_match, glob_filter
)

fn _glob_match_here(p, pi, s, si){
   "Internal: recursive glob matcher for * and ?."
   def p_val = load8(p, pi)
   def s_val = load8(s, si)
   if(p_val==0){ return s_val==0  }
   if(p_val==42){        ; "*"
      pi = pi + 1
      if(load8(p, pi)==0){ return true  }
      def i=0
      while(1){
         if(_glob_match_here(p, pi, s, si+i)){ return true  }
         if(load8(s, si + i)==0){ break  }
         i=i+1
      }
      return false
   }
   if(p_val==63){        ; "?"
      if(s_val==0){ return false  }
      return _glob_match_here(p, pi+1, s, si+1)
   }
   if(p_val!=s_val){ return false  }
   return _glob_match_here(p, pi+1, s, si+1)
}

fn glob_match(pattern, s){
   "Match pattern with * and ? against string s."
   return _glob_match_here(pattern, 0, s, 0)
}

fn glob_filter(pattern, xs){
   "Filter list of strings by glob."
   def out = list(8)
   def i =0  n=list_len(xs)
   while(i<n){
      def v = get(xs, i)
      if(glob_match(pattern, v)){ out = append(out, v)  }
      i=i+1
   }
   return out
}