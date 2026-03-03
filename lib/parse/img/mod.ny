;; Keywords: image format unified
;; Unified image decoding module.
;; Dispatches to format-specific decoders based on signature or file extension.

module std.image (
   decode, get_info, load, save, encode, free,
   FORMAT_AUTO, FORMAT_GREY, FORMAT_GREY_ALPHA, FORMAT_RGB, FORMAT_RGB_ALPHA
)

use std.core *
use std.core.dict_mod *
use std.os *
use std.str as str
use std.image.format.bmp as bmp
use std.image.format.gif as gif
use std.image.format.jpeg as jpeg
use std.image.format.png as png
use std.image.format.tga as tga

def FORMAT_AUTO = 0
def FORMAT_GREY = 1
def FORMAT_GREY_ALPHA = 2
def FORMAT_RGB = 3
def FORMAT_RGB_ALPHA = 4

fn decode(data, ext=""){
   "Decodes an image from bytes. Optional `ext` can help with TGA/BMP dispatch."
   if(!is_str(data) || len(data) < 4){ return 0 }

   def b0 = load8(data, 0)
   def b1 = load8(data, 1)
   def b2 = load8(data, 2)
   def b3 = load8(data, 3)

   ; 1. PNG Signature: 89 50 4E 47
   if(b0 == 137 && b1 == 80 && b2 == 78 && b3 == 71){
      return png.decode(data)
   }

   ; 2. JPEG Signature: FF D8 FF
   if(b0 == 255 && b1 == 216 && b2 == 255){
      return jpeg.decode(data)
   }

   ; 3. BMP Signature: BM (42 4D)
   if(b0 == 66 && b1 == 77){
      return bmp.decode(data)
   }

   ; 4. GIF Signature: GIF8
   if(b0 == 71 && b1 == 73 && b2 == 70 && b3 == 56){
      return gif.decode(data)
   }

   ; 5. Fallback to extension for formats without fixed signatures (like TGA)
   def lext = str.lower(ext)
   if(lext == ".tga"){
      return tga.decode(data)
   } elif(lext == ".bmp"){
      return bmp.decode(data)
   } elif(lext == ".png"){
      return png.decode(data)
   } elif(lext == ".jpg" || lext == ".jpeg"){
      return jpeg.decode(data)
   } elif(lext == ".gif"){
      return gif.decode(data)
   }

   ; Try TGA as last resort if it looks like uncompressed RGB
   if(len(data) > 18){
      def img_type = load8(data, 2)
      if(img_type == 2 || img_type == 3 || img_type == 10 || img_type == 11){
         return tga.decode(data)
      }
   }

   0
}

fn load(path, _req_comp=FORMAT_AUTO){
   "Loads and decodes an image file from `path`."
   def res = file_read(path)
   if(is_ok(res)){
      return decode(unwrap(res), path)
   }
   0
}

fn encode(img, format="bmp"){
   "Encodes an image dictionary into a byte string of the specified `format`."
   if(eq(format, "bmp")){ return bmp.encode(img) }
   if(eq(format, "tga")){ return tga.encode(img) }
   if(eq(format, "png")){ return png.encode(img) }
   if(eq(format, "jpg") || eq(format, "jpeg")){ return jpeg.encode(img) }
   if(eq(format, "gif")){ return gif.encode(img) }
   0
}

fn save(img, path, format="auto"){
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
   def data = encode(img, fmt)
   if(data){
      return file_write(path, data)
   }
   0
}

fn free(img){
   "Frees image resources."
   if(!img || !is_dict(img)){ return }
   def data = dict_get(img, "data", 0)
   if(data){
      __free(data)
      dict_set(img, "data", 0)
   }
}

fn get_info(img){
   "Returns basic info from a loaded image dict."
   if(!is_dict(img)){ return 0 }
   mut info = dict(4)
   info = dict_set(info, "width",  dict_get(img, "width", 0))
   info = dict_set(info, "height", dict_get(img, "height", 0))
   info = dict_set(info, "bpp",    dict_get(img, "bpp", 0))
   info
}

if(comptime{__main()}){
   use std.core.error *

   fn test_dispatch(name, path){
      "Loads `path`, decodes it through the unified image dispatcher, and asserts success when present."
      print("Testing " + name + " dispatch: " + path)
      match file_read(path){
         ok(data) -> {
         def img = decode(data, path)
         assert(img != 0, name + " decode failed")
         print("  ✓ SUCCESS: " + to_str(dict_get(img, "width")) + "x" + to_str(dict_get(img, "height")))
         }
         err(_) -> { print("  ! SKIPPED: " + path + " not found") }
      }
   }

   test_dispatch("PNG", "etc/assets/images/stone.png")
   test_dispatch("JPEG", "etc/assets/images/test.jpg")
   test_dispatch("BMP", "etc/assets/images/test.bmp")
   test_dispatch("TGA", "etc/assets/images/test.tga")

   print("✓ std.image tests passed")
}
