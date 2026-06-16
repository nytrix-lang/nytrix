use std.core
use std.os
use std.os.args
use std.core.str as str
use std.os.path as path
use std.core.glob as glob

fn _input_path(){
   if(argc() > 2){
      def p2 = argv(2)
      if(is_str(p2)){
         if(p2.len > 0){ return p2 }
      }
   }
   if(argc() > 1){
      def p1 = argv(1)
      if(is_str(p1)){
         if(p1.len > 0){ return p1 }
      }
   }
   ""
}

def p = _input_path()

if(len(p) > 0){
   match file_read(p) {
   ok(s) -> {
      def n = path.normalize(s)
      path.basename(n)
      path.dirname(n)
      path.extname(n)
      glob.glob_match("**/*.ny", n)
      glob.glob_match(n, "shapes/syntax/assembly-tokenizer.ny")
      str.split(n, "/")
   }
   err(_) -> { 0 }
   }
}
