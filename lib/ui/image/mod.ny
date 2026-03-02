;; Keywords: image loading

module std.image (
   load, load_mem, save, encode, free,
   FORMAT_AUTO, FORMAT_GREY, FORMAT_GREY_ALPHA, FORMAT_RGB, FORMAT_RGB_ALPHA
)

use std.core *
use std.os *
use std.image.format.bmp as bmp
use std.image.format.tga as tga
use std.image.format.png as png
use std.image.format.jpeg as jpeg
use std.image.format.gif as gif
use std.image.format.ico as ico
use std.text as str

def FORMAT_AUTO = 0
def FORMAT_GREY = 1
def FORMAT_GREY_ALPHA = 2
def FORMAT_RGB = 3
def FORMAT_RGB_ALPHA = 4

fn load_mem(data, size=0, req_comp=FORMAT_AUTO){
   "Decodes image data from memory."
   if(!is_str(data)){ return 0 }
   if(size == 0){ size = len(data) }
   if(size >= 8 && load8(data, 0) == 137 && load8(data, 1) == 80){
      return png.decode(data)
   }
   if(size >= 6 && load8(data, 0) == 71 && load8(data, 1) == 73 && load8(data, 2) == 70){
      return gif.decode(data)
   }
   if(size >= 6 && load8(data, 0) == 0 && load8(data, 1) == 0 && load8(data, 2) == 1 && load8(data, 3) == 0){
      return ico.decode(data)
   }
   if(size >= 2 && load8(data, 0) == 66 && load8(data, 1) == 77){
      return bmp.decode(data)
   }
   if(size >= 18 && (load8(data, 2) == 2 || load8(data, 2) == 3 || load8(data, 2) == 10 || load8(data, 2) == 11)){
      return tga.decode(data)
   }
   if(size >= 2 && load8(data, 0) == 255 && load8(data, 1) == 216){
      return jpeg.decode(data)
   }
   0
}

fn load(path, req_comp=FORMAT_AUTO){
   "Loads and decodes an image file from `path`."
   def data = file_read(path)
   match data {
      err(e) -> {
         print(f"GFX: Failed to read image '{path}': {e}")
         return 0
      }
      ok(buf) -> {
         return load_mem(buf, len(buf), req_comp)
      }
   }
}

fn encode(img, format="bmp"){
   "Encodes an image dictionary into a byte string of the specified `format`."
   if(eq(format, "bmp")){ return bmp.encode(img) }
   if(eq(format, "tga")){ return tga.encode(img) }
   if(eq(format, "png")){ return png.encode(img) }
   if(eq(format, "jpg") || eq(format, "jpeg")){ return jpeg.encode(img) }
   if(eq(format, "gif")){ return gif.encode(img) }
   if(eq(format, "ico")){ return ico.encode(img) }
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
      elif(str.endswith(path, ".ico")){ fmt = "ico" }
      else { fmt = "bmp" }
   }
   def data = encode(img, fmt)
   if(data){
      return file_write(path, data)
   }
   panic("Unsupported image format for saving")
}

fn free(img){
   "Frees image resources. Since Nytrix handles strings/dicts, this mostly frees the 'data' pointer if allocated via malloc."
   if(!img || !is_dict(img)){ return }
   def data = dict_get(img, "data", 0)
   if(data){
      __free(data)
      dict_set(img, "data", 0)
   }
}

if(comptime{__main()}){
   use std.core.error *

   print("Testing std.image dispatcher...")

   fn verify_img(path, name){
      def res = load(path)
      if(!res){
         print("  FAILED: Could not load " + path)
         return false
      }

      def w = get(res, "width")
      def h = get(res, "height")
      if(w != 2 || h != 2){
         print("  FAILED: " + name + " invalid dimensions " + to_str(w) + "x" + to_str(h))
         return false
      }

      def data = get(res, "data")
      def chan = get(res, "channels")

      ;; Check top-left pixel (Red)
      if(chan >= 3){
         def r = load8(data, 0)
         def g = load8(data, 1)
         def b = load8(data, 2)

         ;; For JPEG we allow some error
         mut tolerance = 5
         if(str.contains(name, "JPEG")){ tolerance = 40 }

         if(abs(r - 255) > tolerance || g > tolerance || b > tolerance){
            print("  FAILED: " + name + " pixel(0,0) mismatch: RGB(" + to_str(r) + "," + to_str(g) + "," + to_str(b) + ") expected RGB(255,0,0)")
            return false
         }
      } elif(chan == 1){
         def v = load8(data, 0)
         if(abs(v - 76) > 20){ ;; Gray value for Red is approx 76
            print("  FAILED: " + name + " pixel(0,0) mismatch: Gray(" + to_str(v) + ") expected approx 76")
            return false
         }
      }

      print("  SUCCESS: " + name + " (" + to_str(w) + "x" + to_str(h) + ")")
      true
   }

   mut ok = true
   ok = ok && verify_img("etc/assets/images/test_rgba.png", "PNG RGBA")
   ok = ok && verify_img("etc/assets/images/test_rgb.png", "PNG RGB")
   ok = ok && verify_img("etc/assets/images/test_gray.png", "PNG Gray")
   ok = ok && verify_img("etc/assets/images/test_graya.png", "PNG Gray+Alpha")
   ok = ok && verify_img("etc/assets/images/test_palette.png", "PNG Palette")
   ok = ok && verify_img("etc/assets/images/test.jpg", "JPEG")
   ok = ok && verify_img("etc/assets/images/test_rgba_uncompressed.tga", "TGA RGBA Uncompressed")
   ok = ok && verify_img("etc/assets/images/test_rgba_rle.tga", "TGA RGBA RLE")
   ok = ok && verify_img("etc/assets/images/test_rgb_uncompressed.tga", "TGA RGB Uncompressed")
   ok = ok && verify_img("etc/assets/images/test_rgb_rle.tga", "TGA RGB RLE")
   ok = ok && verify_img("etc/assets/images/test_rgba.bmp", "BMP RGBA")
   ok = ok && verify_img("etc/assets/images/test_rgb.bmp", "BMP RGB")

   if(ok){
      print("✓ std.image all format tests passed")
   } else {
      print("✗ SOME std.image TESTS FAILED")
      __exit(1)
   }
}
