#!/bin/ny
;; Cloc (Example) - Line of Code Counter

use std.core *
use std.str *
use std.str.glob *
use std.str.io *
use std.os.fs *
use std.os.args *
use std.core.reflect *
use std.str.path *

fn count_lines(path){
   def src = file_read(path)
   if(!src){ return 0 }
   mut lc = 0
   for(line in split(src, "\n")){
      def s = strip(line)
      if(len(s)>0 && !startswith(s, ";;") && !startswith(s, "//")){ lc = lc + 1 }
   }
   lc
}

fn do_glob(pattern){
   def d = dirname(pattern)
   if(str_contains(d, "*")){ d = "." }
   def res_box = [list(8)]
   walk(d, fn(p){
      if(is_file(p) && (glob_match(pattern, p) || glob_match(pattern, basename(p)))){
         set_idx(res_box, 0, append(get(res_box, 0), p))
      }
   })
   get(res_box, 0)
}

mut total_lines = 0
mut files = list(8)
def a = args()
if(len(a) > 1){
   for(f in a){ if(f != get(a, 0)){ files = append(files, f) } }
} else {
   for(p in ["**/*.ny", "**/*.c", "**/*.h"]){
      files = extend(files, do_glob(p))
   }
}

for(f in files){
   def lc = count_lines(f)
   print(f"{f}: {lc}")
   total_lines = total_lines + lc
}
print(f"Total lines: {total_lines}")
