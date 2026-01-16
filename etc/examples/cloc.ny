;; Nytrix Line of Code Counter
use std.io *
use std.io.fs *
use std.strings.str *
use std.core

fn count_lines(path){
   def src = file_read(path)
   if(len(src) == 0){ return [0, 0] }
   def code = 0
   def cmts = 0
   for l in split(src, "\n") {
      def line = strip(l)
      if(len(line) > 0){
         if(load8(line) == 34 || load8(line) == 59){
            cmts += 1
         } else {
            code += 1
         }
      }
   }
   return [code, cmts]
}

fn collect_files(dir, out_list){
   def items = listdir(dir)
   if(!items){ return out_list }
   for name in items {
      if(name == "." || name == ".." || name == ".git" || name == "build"){
         continue
      }
      def full = dir + "/" + name
      if(is_dir(full)){
         out_list = collect_files(full, out_list)
      } else {
         if(endswith(full, ".ny") || endswith(full, ".c") || endswith(full, ".h")){
            out_list = append(out_list, full)
         }
      }
   }
   return out_list
}

print(f"{pad_end('File', 40)} Code  Comments")

def all_files = []
all_files = collect_files(".", all_files)
def total_code = 0
def total_cmts = 0
for p in all_files {
   def res = count_lines(p)
   def c = res[0]
   def m = res[1]
   total_code += c
   total_cmts += m
   print(f"{pad_end(p, 40)} {c}    {m}")
}

print(f"{pad_end('TOTAL', 40)} {total_code}    {total_cmts}")
