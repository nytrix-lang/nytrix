;; Keywords: iter itertools
;; Iter Itertools module.

use std.core
use std.collections
module std.iter.itertools (
   product, enumerate, zip, compose, iter_pipe
)

fn product(a,b){
   "Computes the Cartesian product of two lists `a` and `b`. Returns a list of pairs `[x, y]` for every `x` in `a` and `y` in `b`."
   def out = list(8)
   def i =0  na=list_len(a)
   while(i<na){
      def j =0  nb=list_len(b)
      while(j<nb){
         out = append(out, [get(a,i), get(b,j)])
         j=j+1
      }
      i=i+1
   }
   return out
}

fn enumerate(xs){
   "Returns a list of `[index, value]` pairs for each element in `xs`."
   out = list(8)
   i=0  n=list_len(xs)
   while(i<n){
      out = append(out, [i, get(xs,i)])
      i=i+1
   }
   return out
}

fn zip(a,b){
   "Zips two lists `a` and `b` into a list of pairs `[a[i], b[i]]`. Truncates to the length of the shorter list."
   out = list(8)
   na=list_len(a)  nb=list_len(b)
   n = na  if(nb<n){ n=nb  }
   i=0
   while(i<n){
      out = append(out, [get(a,i), get(b,i)])
      i=i+1
   }
   return out
}

fn compose(f, g, x){
   "Composes two unary functions: `compose(f, g, x)` returns `f(g(x))`."
   return f(g(x))
}

fn iter_pipe(x, fs){
   "Pipes value `x` through a list of unary functions `fs`. Returns the result of applying each function in sequence."
   i=0  n=list_len(fs)
   v = x
   while(i<n){
      v = get(fs,i)(v)
      i=i+1
   }
   return v
}