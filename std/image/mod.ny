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
   
   def bmp_h = malloc(14) init_str(bmp_h, 14) store8(bmp_h, 66, 0) store8(bmp_h, 77, 1)
   def png_h = malloc(8) init_str(png_h, 8) store8(png_h, 137, 0) store8(png_h, 80, 1)
   
   load_mem(bmp_h)
   load_mem(png_h)
   
   print("✓ std.image dispatcher tests passed")
}
