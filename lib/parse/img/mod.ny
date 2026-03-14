;; Keywords: image graphics svg vector webp tga gif jpeg jpg png exr openexr hdr bmp bitmap
;; Image codec facade for PNG, JPEG, BMP, GIF, TGA, WebP, SVG, EXR, and image metadata.
module std.parse.img(decode, get_info, load, save, encode, free)
use std.core
use std.core.dict_mod
use std.os
use std.os.thread
use std.os.path as ospath
use std.core.str as str
use std.parse.img.bmp as bmp
use std.parse.img.exr as exr
use std.parse.img.gif as gif
use std.parse.img.jpeg as jpeg
use std.parse.img.png as png
use std.parse.img.svg as svg
use std.parse.img.tga as tga
use std.parse.img.webp as webp
use std.core.common as common

mut _image_decode_mu = 0
mut _image_lock_native_cache = -1
mut _image_disable_native_cache = -1
mut _image_debug_cache = -1

fn _decode_exr_staged(any: data, str: ext=""): any {
   if(!is_str(data) || data.len < 4){ return 0 }
   if(load8(data, 0) != 118 || load8(data, 1) != 47 || load8(data, 2) != 49 || load8(data, 3) != 1){ return 0 }
   def td = temp_dir()
   if(!is_str(td) || td.len == 0){ return 0 }
   def suffix = (is_str(ext) && ext == ".exr") ? ".exr" : ".tmp.exr"
   def path = ospath.join(td, "ny_img_exr_" + to_str(ticks()) + suffix)
   match file_write(path, data){
      ok(ignoredok) -> { ignoredok }
      err(ignorederr) -> { ignorederr  return 0 }
   }
   defer {
      match file_remove(path){ ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
   }
   exr.load_path(path)
}

fn _image_dbg(any: msg): any {
   if(_image_debug_cache < 0){ _image_debug_cache = common.cached_env_truthy(_image_debug_cache, "NY_IMAGE_DEBUG") }
   if(_image_debug_cache == 1){ print("[image] " + msg) }
}

fn _image_decode_mutex(): any {
   if(!_image_decode_mu){ _image_decode_mu = mutex_new() }
   _image_decode_mu
}

fn _image_lock_native_decode(): bool {
   if(_image_lock_native_cache != -1){ return _image_lock_native_cache == 1 }
   def v = common.env_lower("NY_IMAGE_LOCK_NATIVE")
   def on = !(v == "0" || v == "false" || v == "off" || v == "no")
   _image_lock_native_cache = on ? 1 : 0
   on
}

fn _image_native_disabled(): bool {
   if(_image_disable_native_cache < 0){ _image_disable_native_cache = common.cached_env_truthy(_image_disable_native_cache, "NY_IMAGE_DISABLE_NATIVE") }
   _image_disable_native_cache == 1
}

fn _image_decode_dispatch(any: data, str: ext=""): any {
   if(!is_str(data) || data.len < 4){ return 0 }
   def b0, b1 = load8(data, 0), load8(data, 1)
   def b2, b3 = load8(data, 2), load8(data, 3)
   ; 1. PNG Signature: 89 50 4E 47
   if(b0 == 137 && b1 == 80 && b2 == 78 && b3 == 71){ return png.decode(data) }
   ; 2. JPEG Signature: FF D8 FF
   if(b0 == 255 && b1 == 216 && b2 == 255){ return jpeg.decode(data) }
   ; 3. BMP Signature: BM (42 4D)
   if(b0 == 66 && b1 == 77){ return bmp.decode(data) }
   ; 4. GIF Signature: GIF8
   if(b0 == 71 && b1 == 73 && b2 == 70 && b3 == 56){ return gif.decode(data) }
   ; 5. WEBP Signature: RIFF....WEBP
   if(data.len >= 12 && b0 == 82 && b1 == 73 && b2 == 70 && b3 == 70 &&
      load8(data, 8) == 87 && load8(data, 9) == 69 && load8(data, 10) == 66 && load8(data, 11) == 80){
      return webp.decode(data)
   }
   ; 6. OpenEXR signature: 76 2f 31 01
   if(b0 == 118 && b1 == 47 && b2 == 49 && b3 == 1){ return _decode_exr_staged(data, ext) }
   ; 7. SVG XML document
   def svg_try = svg.decode(data, ext)
   if(svg_try){ return svg_try }
   ; 8. Fallback to extension for formats without fixed signatures
   def lext = str.lower(ext)
   if(lext == ".tga"){ return tga.decode(data) } elif(lext == ".bmp"){
      return bmp.decode(data)
   } elif(lext == ".png"){
      return png.decode(data)
   } elif(lext == ".jpg" || lext == ".jpeg"){
      return jpeg.decode(data)
   } elif(lext == ".gif"){
      return gif.decode(data)
   } elif(lext == ".webp"){
      return webp.decode(data)
   } elif(lext == ".exr"){
      return _decode_exr_staged(data, ext)
   } elif(lext == ".svg"){
      return svg.decode(data)
   }
   ; Try TGA as last resort if it looks like uncompressed RGB
   if(data.len > 18){
      def img_type = load8(data, 2)
      if(img_type == 2 || img_type == 3 || img_type == 10 || img_type == 11){ return tga.decode(data) }
   }
   0
}

fn decode(any: data, str: ext=""): any {
   "Decodes an image from bytes. Optional `ext` can help with TGA/BMP dispatch."
   _image_decode_dispatch(data, ext)
}

fn load(str: path, int: _req_comp=0): any {
   "Loads and decodes an image file from `path`."
   def lpath = str.lower(path)
   if(str.endswith(lpath, ".exr")){
      def exr_img = exr.load_path(path)
      if(exr_img){ return exr_img }
      _image_dbg("exr backend failed for " + path)
      return 0
   }
   if(str.endswith(lpath, ".svg")){
      def svg_img = svg.load_path(path)
      if(svg_img){ return svg_img }
      _image_dbg("svg backend failed for " + path + ": " + svg.last_error())
      return 0
   }
   def res = file_read(path)
   if(is_ok(res)){
      def bytes = unwrap(res)
      if(_image_native_disabled()){
         _image_dbg("native disabled for " + path)
         return 0
      }
      mut native = 0
      if(_image_lock_native_decode()){
         def mu = _image_decode_mutex()
         if(mu){ mutex_lock(mu) }
         native = _image_decode_dispatch(bytes, path)
         if(mu){ mutex_unlock(mu) }
      } else {
         native = _image_decode_dispatch(bytes, path)
      }
      if(native){ return native }
      _image_dbg("native failed for " + path)
      return 0
   }
   0
}

fn encode(any: img, str: format="bmp"): any {
   "Encodes an image dictionary into a byte string of the specified `format`."
   if(eq(format, "bmp")){ return bmp.encode(img) }
   if(eq(format, "tga")){ return tga.encode(img) }
   if(eq(format, "png")){ return png.encode(img) }
   if(eq(format, "jpg") || eq(format, "jpeg")){ return jpeg.encode(img) }
   if(eq(format, "gif")){ return gif.encode(img) }
   0
}

fn save(any: img, str: path, str: format="auto"): any {
   "Saves an image object to a file."
   mut fmt = format
   if(eq(fmt, "auto")){
      if(str.endswith(path, ".bmp")){ fmt = "bmp" }
      elif(str.endswith(path, ".tga")){ fmt = "tga" }
      elif(str.endswith(path, ".png")){ fmt = "png" }
      elif(str.endswith(path, ".jpg") || str.endswith(path, ".jpeg")){ fmt = "jpeg" }
      elif(str.endswith(path, ".gif")){ fmt = "gif" }
      else { fmt = "bmp" }
   }
   if(eq(fmt, "tga")){ return tga.save(img, path) }
   def data = encode(img, fmt)
   if(data){ return file_write(path, data) }
   0
}

fn free(any: img): any {
   "Decoded image payloads are stored as Ny strings via init_str(...).
   Manual freeing here corrupts ownership because callers still hold dict refs
   and the runtime will release the string storage on its own."
   if(!img || !is_dict(img)){ return nil }
   nil
}

fn get_info(any: img): any {
   "Returns basic info from a loaded image dict."
   if(!is_dict(img)){ return 0 }
   mut info = dict(4)
   info = info.set("width",  img.get("width", 0))
   info = info.set("height", img.get("height", 0))
   info = info.set("bpp",    img.get("bpp", 0))
   info
}
