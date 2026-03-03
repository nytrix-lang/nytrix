;; Keywords: enc 3d obj mesh
;; Simple Wavefront OBJ loader for std.ui.gfx.

module std.parse.threed.obj (
   load_obj, mesh_from_obj, parse_obj_str
)

use std.core *
use std.str *
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
             vs = append(vs, [atof(get(clean_parts, 1)), atof(get(clean_parts, 2)), atof(get(clean_parts, 3))])
         }
      } elif(eq(cmd, "vn")){
         if(len(clean_parts) >= 4){
             vns = append(vns, [atof(get(clean_parts, 1)), atof(get(clean_parts, 2)), atof(get(clean_parts, 3))])
         }
      } elif(eq(cmd, "vt")){
         if(len(clean_parts) >= 3){
             vts = append(vts, [atof(get(clean_parts, 1)), atof(get(clean_parts, 2))])
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

fn _calc_normal(p0, p1, p2){
   "Internal: calculates a face normal from 3 points."
   def v1 = v_sub(p1, p0)
   def v2 = v_sub(p2, p0)
   normalize(cross3(v1, v2))
}

fn mesh_from_obj(obj_data){
   "Converts OBJ data into a flat list of vertex entries [[x,y,z], [u,v], [nx,ny,nz]]."
   if(!obj_data){ return list(0) }
   def vs = dict_get(obj_data, "vertices")
   def vns = dict_get(obj_data, "normals")
   def vts = dict_get(obj_data, "uvs")
   def fs = dict_get(obj_data, "faces")

   mut out = list()
   mut i = 0
   while(i < len(fs)){
      def face = get(fs, i)
      if(len(face) >= 3){
         mut face_normal = [0.0, 1.0, 0.0]
         mut has_face_normal = false

         def _get_vert = fn(idx_info){
         def vi = get(idx_info, 0) - 1
         def vti = get(idx_info, 1) - 1
         def vni = get(idx_info, 2) - 1

         def p = get(vs, vi, [0.0, 0.0, 0.0])
         mut uv = [0.0, 0.0]
         if(vti >= 0 && vti < len(vts)){ uv = get(vts, vti) }

         mut n = face_normal
         if(vni >= 0 && vni < len(vns)){ n = get(vns, vni) }
         elif(has_face_normal){ n = face_normal }

         [p, uv, n]
         }

         ; If no normals in file, calculate face normal
         def v0_idx_info = get(face, 0)
         if(get(v0_idx_info, 2) <= 0){
         def p0 = get(vs, get(get(face, 0), 0) - 1)
         def p1 = get(vs, get(get(face, 1), 0) - 1)
         def p2 = get(vs, get(get(face, 2), 0) - 1)
         face_normal = _calc_normal(p0, p1, p2)
         has_face_normal = true
         }

         def v0 = _get_vert(get(face, 0))
         mut j = 1
         while(j + 1 < len(face)){
         out = append(out, v0)
         out = append(out, _get_vert(get(face, j)))
         out = append(out, _get_vert(get(face, j+1)))
         j += 1
         }
      }
      i += 1
   }
   out
}
