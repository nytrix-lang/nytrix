use std.core
use std.os
use std.os.args
use std.math.parse.3d.gltf as gltf

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
   ok(raw) -> {
      def parsed = gltf.parse_gltf_str(raw)
      if(is_dict(parsed)){ len(dict_keys(parsed)) }
      def from_file = gltf.load_gltf_file(p)
      if(is_dict(from_file)){ len(dict_keys(from_file)) }
   }
   err(_) -> { 0 }
   }
}
