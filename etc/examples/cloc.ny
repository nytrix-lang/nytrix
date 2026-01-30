#!/bin/ny
use std.core *
use std.str *
use std.str.glob *
use std.str.io *
use std.os.fs *
use std.os.args *
use std.core.reflect *
use std.str.path *

;; Cloc (Example) - Line of Code Counter

fn count_lines(path){
   def src = file_read(path)
   if(!src){ return 0 }
   mut lc = 0
   for(line in split(src, "\n")){
      def s = strip(line)
      if(len(s)>0 && !startswith(s, ";;")){ lc = lc + 1 }
   }
   lc
}

fn do_glob(pattern){
   def res_box = [list(8)]
   walk(".", fn(p){
      if(is_file(p)){
         if(glob_match(pattern, p) || glob_match(pattern, basename(p))){
            def curr = get(res_box, 0)
            set_idx(res_box, 0, append(curr, p))
         }
      }
   })
   get(res_box, 0)
}

fn main(){
   mut total_lines = 0
   mut files = []
   def a = args()
   if(len(a) > 1){
      for(f in a){
         if(f != get(a, 0)){
            files = append(files, f)
         }
      }
   } else {
      files = do_glob("**/*.ny")
   }

   for(f in files){
      def lc = count_lines(f)
      print(f"{f}: {lc}")
      total_lines = total_lines + lc
   }

   print(f"Total lines: {total_lines}")
}

main()

