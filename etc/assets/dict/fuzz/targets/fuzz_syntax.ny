use std.core
use std.os
use std.os.args
use std.math.parse.syntax as syn

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
      def s = to_str(raw)
      if(endswith(p, "CMakeLists.txt") || endswith(p, ".cmake")){
         0
      } else {
      syn.tokenize_auto(s, p, list(0))
      if(endswith(p, ".ny")){ syn.nytrix_tokenize(s, list(0)) }
      elif(endswith(p, ".c")){ syn.c_tokenize(s, list(0)) }
      elif(endswith(p, ".py")){ syn.python_tokenize(s, list(0)) }
      elif(endswith(p, ".js")){ syn.javascript_tokenize(s, list(0)) }
      elif(endswith(p, ".ts")){ syn.typescript_tokenize(s, list(0)) }
      elif(endswith(p, ".lua")){ syn.lua_tokenize(s, list(0)) }
      elif(endswith(p, ".sh")){ syn.bash_tokenize(s, list(0)) }
      elif(endswith(p, "CMakeLists.txt") || endswith(p, ".cmake")){ syn.cmake_tokenize(s, list(0)) }
      elif(endswith(p, ".yaml") || endswith(p, ".yml")){ syn.yaml_tokenize(s, list(0)) }
      elif(endswith(p, ".xml")){ syn.xml_tokenize(s, list(0)) }
      elif(endswith(p, ".html")){ syn.html_tokenize(s, list(0)) }
      elif(endswith(p, ".md")){ syn.markdown_tokenize(s, list(0)) }
      elif(endswith(p, ".s") || endswith(p, ".asm")){ syn.assembly_tokenize(s, list(0)) }
      elif(endswith(p, ".json")){ syn.json_tokenize(s, list(0)) }
      }
      0
   }
   err(_) -> { 0 }
   }
}
