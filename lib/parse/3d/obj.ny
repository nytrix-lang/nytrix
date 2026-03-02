;; Keywords: enc 3d obj mesh
;; Simple Wavefront OBJ loader for std.ui.gfx.

module std.parse.threed.obj (
   load_obj, mesh_from_obj, parse_obj_str
)

use std.core *
use std.text *
use std.math.vector *

fn parse_obj_str(content){
   "Parses an OBJ string and returns { vertices, normals, uvs, faces }."
   def lines = split(replace_all(content, "\r\n", "\n"), "\n")

   mut vs = list()
   mut vns = list()
   mut vts = list()
   mut fs = list()

   mut i = 0
   while(i < len(lines)){
      def line = strip(get(lines, i))
      if(str_len(line) < 2 || load8(line, 0) == 35){ i += 1 continue }

      def parts = split(line, " ")
      mut clean_parts = list()
      mut k = 0 while(k < len(parts)){
         def p = strip(get(parts, k))
         if(str_len(p) > 0){ clean_parts = append(clean_parts, p) }
         k += 1
      }
      if(len(clean_parts) < 2){ i += 1 continue }
      
      def cmd = get(clean_parts, 0)

      if(eq(cmd, "v")){
         if(len(clean_parts) >= 4){
             vs = append(vs, [float(get(clean_parts, 1)), float(get(clean_parts, 2)), float(get(clean_parts, 3))])
         }
      } elif(eq(cmd, "vn")){
         if(len(clean_parts) >= 4){
             vns = append(vns, [float(get(clean_parts, 1)), float(get(clean_parts, 2)), float(get(clean_parts, 3))])
         }
      } elif(eq(cmd, "vt")){
         if(len(clean_parts) >= 3){
             vts = append(vts, [float(get(clean_parts, 1)), float(get(clean_parts, 2))])
         }
      } elif(eq(cmd, "f")){
         mut f = list()
         mut j = 1 while(j < len(clean_parts)){
            def p = get(clean_parts, j)
            def sub = split(p, "/")
            def v_idx = atoi(get(sub, 0))
            mut vt_idx = 0
            mut vn_idx = 0
            if(len(sub) > 1 && str_len(get(sub, 1)) > 0){ vt_idx = atoi(get(sub, 1)) }
            if(len(sub) > 2 && str_len(get(sub, 2)) > 0){ vn_idx = atoi(get(sub, 2)) }
            f = append(f, [v_idx, vt_idx, vn_idx])
            j += 1
         }
         fs = append(fs, f)
      }
      i += 1
   }

   mut out = dict(8)
   out = dict_set(out, "vertices", vs)
   out = dict_set(out, "normals", vns)
   out = dict_set(out, "uvs", vts)
   out = dict_set(out, "faces", fs)
   out
}

fn load_obj(path){
   "Loads a Wavefront OBJ file and returns { vertices, normals, uvs, faces }."
   def res = file_read(path)
   if(is_err(res)){ return 0 }
   parse_obj_str(unwrap(res))
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
         def v0_info = get(face, 0)
         def v0_idx = get(v0_info, 0) - 1
         if(v0_idx < 0 || v0_idx >= len(vs)){ i += 1 continue }
         def v0 = get(vs, v0_idx)

         mut j = 1
         while(j + 1 < len(face)){
            def v1_info = get(face, j)
            def v2_info = get(face, j+1)
            def v1_idx = get(v1_info, 0) - 1
            def v2_idx = get(v2_info, 0) - 1
            if(v1_idx >= 0 && v1_idx < len(vs) && v2_idx >= 0 && v2_idx < len(vs)){
                out = append(out, v0)
                out = append(out, get(vs, v1_idx))
                out = append(out, get(vs, v2_idx))
            }
            j += 1
         }
      }
      i += 1
   }
   out
}

if(comptime{__main()}){
   use std.core *
   use std.parse.threed.obj *
   print("Testing std.parse.threed.obj...")
   def cube_res = file_read("etc/assets/model/cube.obj")
   assert(is_ok(cube_res), "cube.obj exists")
   def cube = unwrap(cube_res)
   def data = parse_obj_str(cube)
   assert(len(dict_get(data, "vertices")) == 4, "4 vertices")
   assert(len(dict_get(data, "faces")) == 1, "1 face")
   
   def mesh = mesh_from_obj(data)
   assert(len(mesh) == 6, "1 quad -> 2 triangles -> 6 vertices")
   print("✓ std.parse.threed.obj tests passed")
}
