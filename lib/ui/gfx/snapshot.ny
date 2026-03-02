;; Keywords: ui gfx snapshot tga
;; Utilty for saving TGA images from raw buffers.

module std.ui.gfx.snapshot (
   save_tga
)

use std.core *

fn save_tga(filename, buf, w, h){
   "Saves a raw RGBA buffer as a TGA file."
   def f = fopen(filename, "wb")
   if(!f){ return false }
   ; TGA Header (18 bytes)
   mut header = malloc(18)
   memset(header, 0, 18)
   store8(header, 2, 2)     ; Uncompressed true-color image
   store16(header, w, 12)   ; Width
   store16(header, h, 14)   ; Height
   store8(header, 32, 16)   ; Bits per pixel
   store8(header, 0x20, 17) ; Top-left origin
   fwrite(header, 1, 18, f)
   free(header)
   ; Convert RGBA to BGRA (TGA expectation) or just use the raw buffer if it matches.
   ; Most Nytrix backends use BGRA or RGBA. Let's assume input is RGBA and we want BGRA.
   def size = w * h * 4
   mut bgra = malloc(size)
   mut i = 0
   while(i < size){
      store8(bgra, load8(buf, i + 2), i)     ; B
      store8(bgra, load8(buf, i + 1), i + 1) ; G
      store8(bgra, load8(buf, i + 0), i + 2) ; R
      store8(bgra, load8(buf, i + 3), i + 3) ; A
      i += 4
   }
   fwrite(bgra, 1, size, f)
   free(bgra)
   fclose(f)
   true
}
