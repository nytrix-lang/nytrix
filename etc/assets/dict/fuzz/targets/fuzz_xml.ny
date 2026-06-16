use std.core
use std.os
use std.os.args
use std.math.parse.data.xml as xml

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
      def n = xml.parse(s)
      if(is_dict(n)){
         def rt = xml.encode(n)
         len(rt)
      }
   }
   err(_) -> { 0 }
   }
}
