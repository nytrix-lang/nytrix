use std.core
use std.os
use std.os.args
use std.math.parse.data.csv as csv

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
      def rows = csv.decode(s)
      if(is_list(rows)){
         def out = csv.encode(rows)
         len(out)
      }
   }
   err(_) -> { 0 }
   }
}
