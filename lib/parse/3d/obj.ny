;; Keywords: 3d obj wavefront
;; Wavefront OBJ mesh loading for std.os.ui.render workflows.
;; Reference:
;; - https://en.wikipedia.org/wiki/Wavefront_.obj_file
;; - https://github.com/syoyo/tinyobjloader-c
module std.parse.3d.obj(load_obj, mesh_from_obj, parse_obj_str)
use std.core
use std.core.str
use std.math.vector
use std.os (file_read)

fn parse_obj_str(str: content): dict {
   "Parses an OBJ string and returns { vertices, normals, uvs, faces }."
   def lines = split(str_replace(content, "\r\n", "\n"), "\n")
   mut vs = list()
   mut vns = list()
   mut vts = list()
   mut fs = list()
   def lines_n = lines.len
   mut i = 0
   while(i < lines_n){
      def line = strip(lines.get(i))
      if(line.len < 2 || load8(line, 0) == 35){ i += 1 continue }
      def parts = split_words(line)
      if(parts.len < 2){ i += 1 continue }
      def cmd = parts.get(0)
      if(eq(cmd, "v")){
         if(parts.len >= 4){ vs = vs.append([atof(parts.get(1)), atof(parts.get(2)), atof(parts.get(3))]) }
      } elif(eq(cmd, "vn")){
         if(parts.len >= 4){ vns = vns.append([atof(parts.get(1)), atof(parts.get(2)), atof(parts.get(3))]) }
      } elif(eq(cmd, "vt")){
         if(parts.len >= 3){ vts = vts.append([atof(parts.get(1)), atof(parts.get(2))]) }
      } elif(eq(cmd, "f")){
         mut f = list()
         def parts_n = parts.len
         mut j = 1 while(j < parts_n){
            def p = parts.get(j)
            def sub = split(p, "/")
            def v_idx = atoi(sub.get(0))
            mut vt_idx, vn_idx = 0, 0
            if(sub.len > 1 && len(sub.get(1)) > 0){ vt_idx = atoi(sub.get(1)) }
            if(sub.len > 2 && len(sub.get(2)) > 0){ vn_idx = atoi(sub.get(2)) }
            f = f.append([v_idx, vt_idx, vn_idx])
            j += 1
         }
         fs = fs.append(f)
      }
      i += 1
   }
   {"vertices": vs, "normals": vns, "uvs": vts, "faces": fs}
}

fn load_obj(str: path): any {
   "Loads a Wavefront OBJ file and returns { vertices, normals, uvs, faces }."
   def res = file_read(path)
   if(is_err(res)){ return 0 }
   parse_obj_str(unwrap(res))
}

fn _calc_normal(list: p0, list: p1, list: p2): list {
   def v1, v2 = v_sub(p1, p0), v_sub(p2, p0)
   normalize(cross3(v1, v2))
}

fn _obj_vertex_entry(list: idx_info, list: vs, list: vts, list: vns, list: face_normal, bool: has_face_normal): list {
   def vi = idx_info.get(0) - 1
   def vti = idx_info.get(1) - 1
   def vni = idx_info.get(2) - 1
   def p = vs.get(vi, [0.0, 0.0, 0.0])
   mut uv = [0.0, 0.0]
   if(vti >= 0 && vti < vts.len){ uv = vts.get(vti) }
   mut n = face_normal
   if(vni >= 0 && vni < vns.len){ n = vns.get(vni) }
   elif(has_face_normal){ n = face_normal }
   [p, uv, n]
}

fn mesh_from_obj(any: obj_data): list {
   "Converts OBJ data into a flat list of vertex entries [[x,y,z], [u,v], [nx,ny,nz]]."
   if(!obj_data){ return list(0) }
   def vs = obj_data.get("vertices")
   def vns = obj_data.get("normals")
   def vts = obj_data.get("uvs")
   def fs = obj_data.get("faces")
   mut out = list()
   def fs_n = fs.len
   mut i = 0
   while(i < fs_n){
      def face = fs.get(i)
      def face_n = face.len
      if(face_n >= 3){
         mut face_normal = [0.0, 1.0, 0.0]
         mut has_face_normal = false
         ; If no normals in file, calculate face normal
         def v0_idx_info = face.get(0)
         if(v0_idx_info.get(2) <= 0){
            def p0, p1 = vs.get(face.get(0).get(0) - 1), vs.get(face.get(1).get(0) - 1)
            def p2 = vs.get(face.get(2).get(0) - 1)
            face_normal = _calc_normal(p0, p1, p2)
            has_face_normal = true
         }
         def v0 = _obj_vertex_entry(face.get(0), vs, vts, vns, face_normal, has_face_normal)
         mut j = 1
         while(j + 1 < face_n){
            out = out.append(v0)
            out = out.append(_obj_vertex_entry(face.get(j), vs, vts, vns, face_normal, has_face_normal))
            out = out.append(_obj_vertex_entry(face.get(j+1), vs, vts, vns, face_normal, has_face_normal))
            j += 1
         }
      }
      i += 1
   }
   out
}
