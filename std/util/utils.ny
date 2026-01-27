;; Keywords: util utils
;; General utility module.

use std.core *
use std.math.random *
use std.str.io *
use std.str *
use std.core.reflect *
use std.core.dict *
use std.core.error *
use std.str.json *

module std.util.utils (
   parse_ast,
   counter, counter_add, most_common
)

fn parse_ast(source){
   "Parses Nytrix source code and returns the AST as a nested structure (list of dicts)."
   mut json_str = __parse_ast(source)
   if(json_str == 0){ return 0 }
   def ast = json_decode(json_str)
   free(json_str)
   return ast
}

fn counter(xs){
   "Creates a frequency counter dictionary from the elements of list or string `xs`."
   mut d = dict(16)
   mut i = 0  def n = len(xs)
   while(i < n){
      def v = get(xs, i)
      def c = dict_get(d, v, 0)
      d = dict_set(d, v, c + 1)
      i = i + 1
   }
   return d
}

fn counter_add(d, key, n){
   "Adds `n` to the count of `key` in counter dictionary `d`."
   def c = dict_get(d, key, 0)
   return dict_set(d, key, c + n)
}

fn most_common(d){
   "Returns a list of `[item, count]` pairs from counter `d`, sorted by count in descending order."
   def its = items(d)
   print(f"DEBUG: most_common its={its}")
   def n = len(its)
   ; selection sort by count desc
   mut i = 0
   while(i < n){
      mut max_idx = i
      mut j = i + 1
      while(j < n){
         def count_j = get(get(its, j), 1)
         def count_max = get(get(its, max_idx), 1)
         if(count_j > count_max){
            max_idx = j
         }
         j = j + 1
      }
      if(max_idx != i){
         def tmp = get(its, i)
         store_item(its, i, get(its, max_idx))
         store_item(its, max_idx, tmp)
      }
      i = i + 1
   }
   return its
}
