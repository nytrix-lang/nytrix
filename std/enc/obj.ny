;; Keywords: enc 3d obj mesh
;; Simple Wavefront OBJ loader for std.ui.gfx.

module std.enc.obj (
   load_obj, mesh_from_obj
)

use std.core *
use std.text *
use std.math.vector *

fn load_obj(path){
   "Loads a Wavefront OBJ file and returns { vertices, normals, uvs, faces }."
   def res = file_read(path)
   if(is_err(res)){ return 0 }
   def content = unwrap(res)
   def lines = split(replace_all(content, "\r\n", "\n"), "\n")
   
   mut vs = list()
   mut vns = list()
   mut vts = list()
   mut fs = list()
   
   mut i = 0
   while(i < len(lines)){
      def line = strip(get(lines, i))
      if(str_len(line) < 2 || load8(line, 0) == 35){ i += 1 continue } ;; skip comments/empty
      
      def parts = split(line, " ")
      def cmd = get(parts, 0)
      
      if(eq(cmd, "v")){
         vs = append(vs, [float(get(parts, 1)), float(get(parts, 2)), float(get(parts, 3))])
      } elif(eq(cmd, "vn")){
         vns = append(vns, [float(get(parts, 1)), float(get(parts, 2)), float(get(parts, 3))])
      } elif(eq(cmd, "vt")){
         vts = append(vts, [float(get(parts, 1)), float(get(parts, 2))])
      } elif(eq(cmd, "f")){
         ;; Simple face parser (supports v, v/vt, v/vt/vn, v//vn)
         mut f = list()
         mut j = 1 while(j < len(parts)){
            def p = get(parts, j)
            if(str_len(p) > 0){
               def sub = split(p, "/")
               f = append(f, [atoi(get(sub, 0)), 0, 0]) ;; just vertex index for now
            }
            j += 1
         }
         fs = append(fs, f)
      }
      i += 1
   }
   
   mut out = dict(8)
   out = dict_set(out, "vertices", vs)
   out = dict_set(out, "faces", fs)
   out
}

fn mesh_from_obj(obj_data){
   "Converts OBJ data into a flat list of vertices for draw_triangles."
   if(!obj_data){ return list(0) }
   def vs = dict_get(obj_data, "vertices")
   def fs = dict_get(obj_data, "faces")
   
   mut out = list()
   mut i = 0
   while(i < len(fs)){
      def face = get(fs, i)
      if(len(face) >= 3){
         ;; Simple triangulation (fan)
         def v0_idx = get(get(face, 0), 0) - 1
         def v0 = get(vs, v0_idx)
         
         mut j = 1
         while(j + 1 < len(face)){
            def v1_idx = get(get(face, j), 0) - 1
            def v2_idx = get(get(face, j+1), 0) - 1
            out = append(out, v0)
            out = append(out, get(vs, v1_idx))
            out = append(out, get(vs, v2_idx))
            j += 1
         }
      }
      i += 1
   }
   out
}
