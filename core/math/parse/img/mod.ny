;; Keywords: image graphics svg vector webp tga gif jpeg jpg png exr openexr hdr bmp bitmap parse
;; Image codec facade for PNG, JPEG, BMP, GIF, TGA, WebP, SVG, EXR, and image metadata.
;; References:
;; - core.math.parse
module core.math.parse.img(decode, get_info, load, save, encode, free, create, crop, resize, dither, canvas, flip, rotate90, channel, invert, checked, grayscale, tint, brightness, contrast, replace_color, alpha_premultiply, alpha_mask, alpha_crop, alpha_clear, alpha_bounds, pixel, rgba_colors, palette, draw_pixel, draw_rect, draw_line, draw_circle, draw_rect_outline, draw_circle_outline, draw_triangle, draw_triangle_outline, draw_triangle_gradient, draw_rect_gradient, draw_circle_gradient, draw_image, draw_image_scaled, draw_image_ex, draw_text, gradient_linear, gradient_radial, gradient_square, rotate, convolve, blur, white_noise, perlin_noise, cellular, convert_channels, to_pot, mipmaps, pack16, unpack16)
use core
use core.dict
use core.os
use core.os.thread
use core.os.path as ospath
use core.str as str
use core.math.parse.img.bmp as bmp
use core.math.parse.img.exr as exr
use core.math.parse.img.gif as gif
use core.math.parse.img.jpeg as jpeg
use core.math.parse.img.png as png
use core.math.parse.img.svg as svg
use core.math.parse.img.tga as tga
use core.math.parse.img.webp as webp
use core.math.parse.img.ops as ops
use core.common as common

mut _image_decode_mu = 0
mut _image_lock_native_cache = -1
mut _image_disable_native_cache = -1
mut _image_debug_cache = -1

fn create(int width, int height, list color) any { "Creates a packed 8-bit image filled with color." ops.create(width, height, color) }

fn crop(any image, int x, int y, int width, int height) any { "Copies an in-bounds rectangle." ops.crop(image, x, y, width, height) }

fn resize(any image, int width, int height, str filter="bilinear") any { "Resizes with bilinear or nearest filtering." ops.resize(image, width, height, filter) }

fn dither(any image, int bits) any { "Quantizes color channels with Floyd-Steinberg error diffusion." ops.dither(image, bits) }

fn canvas(any image, int width, int height, int x, int y, list color) any { "Copies an image onto a filled canvas." ops.canvas(image, width, height, x, y, color) }

fn flip(any image, bool horizontal, bool vertical) any { "Mirrors an image." ops.flip(image, horizontal, vertical) }

fn rotate90(any image, bool clockwise=true) any { "Rotates an image by 90 degrees." ops.rotate90(image, clockwise) }

fn channel(any image, int selected) any { "Extracts one image channel." ops.channel(image, selected) }

fn invert(any image) any { "Inverts color channels, preserving alpha." ops.invert(image) }

fn checked(int width, int height, int cell_width, int cell_height, list first, list second) any { "Creates a checkerboard image." ops.checked(width, height, cell_width, cell_height, first, second) }

fn grayscale(any image) any { "Converts to luminance while preserving alpha." ops.grayscale(image) }

fn tint(any image, list color) any { "Multiplies pixels by an 8-bit tint." ops.tint(image, color) }

fn brightness(any image, int amount) any { "Adjusts brightness by -255..255." ops.brightness(image, amount) }

fn contrast(any image, int amount) any { "Adjusts contrast by -100..100." ops.contrast(image, amount) }

fn replace_color(any image, list from, list to) any { "Replaces exact packed pixel colors." ops.replace_color(image, from, to) }

fn alpha_premultiply(any image) any { "Premultiplies color channels by alpha." ops.alpha_premultiply(image) }

fn alpha_mask(any image, any mask) any { "Applies a matching grayscale/gray-alpha mask." ops.alpha_mask(image, mask) }

fn alpha_crop(any image, f64 threshold=0.0) any { "Crops to visible alpha bounds." ops.alpha_crop(image, threshold) }

fn alpha_clear(any image, list color, f64 threshold) any { "Replaces pixels whose alpha is below a threshold." ops.alpha_clear(image, color, threshold) }

fn alpha_bounds(any image, f64 threshold=0.0) any { "Returns visible alpha bounds as x/y/width/height." ops.alpha_bounds(image, threshold) }

fn pixel(any image, int x, int y) any { "Returns pixel channels or 0 outside image bounds." ops.pixel(image, x, y) }

fn rgba_colors(any image) any { "Returns row-major RGBA byte lists." ops.rgba_colors(image) }

fn palette(any image, int max_colors) any { "Returns unique visible RGBA colors up to a limit." ops.palette(image, max_colors) }

fn draw_pixel(any image, int x, int y, list color) any { "Returns a copy with one pixel drawn." ops.draw_pixel(image, x, y, color) }

fn draw_rect(any image, int x, int y, int width, int height, list color) any { "Returns a copy with a clipped filled rectangle." ops.draw_rect(image, x, y, width, height, color) }

fn draw_line(any image, int x0, int y0, int x1, int y1, list color) any { "Returns a copy with a clipped line drawn." ops.draw_line(image, x0, y0, x1, y1, color) }

fn draw_circle(any image, int center_x, int center_y, int radius, list color) any { "Returns a copy with a clipped filled circle." ops.draw_circle(image, center_x, center_y, radius, color) }

fn draw_rect_outline(any image, int x, int y, int width, int height, list color) any { "Returns a copy with a clipped rectangle outline." ops.draw_rect_outline(image, x, y, width, height, color) }

fn draw_circle_outline(any image, int center_x, int center_y, int radius, list color) any { "Returns a copy with a clipped circle outline." ops.draw_circle_outline(image, center_x, center_y, radius, color) }

fn draw_image(any image, any source, int x, int y, list tint=[255, 255, 255, 255]) any { "Alpha-composites a clipped source image into a copy." ops.draw_image(image, source, x, y, tint) }

fn draw_triangle(any image, int x1, int y1, int x2, int y2, int x3, int y3, list color) any { "Returns a copy with a clipped filled triangle." ops.draw_triangle(image, x1, y1, x2, y2, x3, y3, color) }

fn draw_triangle_outline(any image, int x1, int y1, int x2, int y2, int x3, int y3, list color) any { "Returns a copy with clipped triangle edges." ops.draw_triangle_outline(image, x1, y1, x2, y2, x3, y3, color) }

fn draw_triangle_gradient(any image, int x1, int y1, list color1, int x2, int y2, list color2, int x3, int y3, list color3) any { "Returns a copy with interpolated triangle colors." ops.draw_triangle_gradient(image, x1, y1, color1, x2, y2, color2, x3, y3, color3) }

fn draw_rect_gradient(any image, f64 x, f64 y, f64 width, f64 height, list top_left, list bottom_left, list bottom_right, list top_right) any { "Returns a copy with a four-corner color gradient." ops.draw_rect_gradient(image, x, y, width, height, top_left, bottom_left, bottom_right, top_right) }

fn draw_circle_gradient(any image, f64 center_x, f64 center_y, f64 radius, list inner, list outer) any { "Returns a copy with a radial color gradient." ops.draw_circle_gradient(image, center_x, center_y, radius, inner, outer) }

fn draw_image_scaled(any image, any source, int x, int y, int width, int height, list tint=[255, 255, 255, 255]) any { "Alpha-composites a nearest-scaled image." ops.draw_image_scaled(image, source, x, y, width, height, tint) }

fn draw_image_ex(any image, any source, int x, int y, f64 rotation, f64 scale=1.0, list tint=[255, 255, 255, 255]) any { "Alpha-composites a nearest-sampled rotated/scaled image." ops.draw_image_ex(image, source, x, y, rotation, scale, tint) }

fn draw_text(any image, any font, str text, int x, int y, int pixel_size, list color=[255, 255, 255, 255]) any { "Draws TrueType text at a font line-box position into an image copy." ops.draw_text(image, font, text, x, y, pixel_size, color) }

fn gradient_linear(int width, int height, int direction, list start, list end) any { "Generates a directional RGBA gradient." ops.gradient_linear(width, height, direction, start, end) }

fn gradient_radial(int width, int height, f64 density, list inner, list outer) any { "Generates a radial RGBA gradient." ops.gradient_radial(width, height, density, inner, outer) }

fn gradient_square(int width, int height, f64 density, list inner, list outer) any { "Generates a square RGBA gradient." ops.gradient_square(width, height, density, inner, outer) }

fn rotate(any image, int degrees) any { "Rotates into expanded bounds with bilinear sampling." ops.rotate(image, degrees) }

fn convolve(any image, list kernel) any { "Applies a square floating-point convolution kernel." ops.convolve(image, kernel) }

fn blur(any image, int radius) any { "Applies a four-pass Gaussian-approximation box blur." ops.blur(image, radius) }

fn white_noise(int width, int height, f64 factor, int seed=0) any { "Generates deterministic black/white noise." ops.white_noise(width, height, factor, seed) }

fn perlin_noise(int width, int height, int offset_x, int offset_y, f64 scale, int seed=0) any { "Generates seeded six-octave Perlin noise." ops.perlin_noise(width, height, offset_x, offset_y, scale, seed) }

fn cellular(int width, int height, int tile_size, int seed=0) any { "Generates seeded cellular noise." ops.cellular(width, height, tile_size, seed) }

fn convert_channels(any image, int channels) any { "Converts between packed grayscale, gray-alpha, RGB and RGBA." ops.convert_channels(image, channels) }

fn to_pot(any image, list fill) any { "Pads the image to power-of-two dimensions." ops.to_pot(image, fill) }

fn mipmaps(any image) any { "Returns the image and its successive bilinear mip levels." ops.mipmaps(image) }

fn pack16(any image, str format) any { "Packs pixels as little-endian RGB565, RGBA5551 or RGBA4444." ops.pack16(image, format) }

fn unpack16(any packed_image) any { "Converts a packed 16-bit image back to RGBA8." ops.unpack16(packed_image) }

fn _decode_exr_staged(any data, str ext="") any {
   if !is_str(data) || data.len < 4 { return 0 }
   if load8(data, 0) != 118 || load8(data, 1) != 47 || load8(data, 2) != 49 || load8(data, 3) != 1 { return 0 }
   def td = temp_dir()
   if !is_str(td) || td.len == 0 { return 0 }
   def suffix = (is_str(ext) && ext == ".exr") ? ".exr" : ".tmp.exr"
   def path = ospath.join(td, "ny_img_exr_" + to_str(ticks()) + suffix)
   match file_write(path, data) {
      ok(ignoredok) -> { ignoredok }
      err(ignorederr) -> { ignorederr  return 0 }
   }
   defer {
      match file_remove(path) { ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
   }
   exr.load_path(path)
}

fn _image_dbg(any msg) any {
   if _image_debug_cache < 0 { _image_debug_cache = common.cached_env_truthy(_image_debug_cache, "NY_IMAGE_DEBUG") }
   if _image_debug_cache == 1 { print("[image] " + msg) }
}

fn _image_decode_mutex() any {
   if !_image_decode_mu { _image_decode_mu = mutex_new() }
   _image_decode_mu
}

fn _image_lock_native_decode() bool {
   if _image_lock_native_cache != -1 { return _image_lock_native_cache == 1 }
   def v = common.env_lower("NY_IMAGE_LOCK_NATIVE")
   def on = !(v == "0" || v == "false" || v == "off" || v == "no")
   _image_lock_native_cache = on ? 1 : 0
   on
}

fn _image_native_disabled() bool {
   if _image_disable_native_cache < 0 { _image_disable_native_cache = common.cached_env_truthy(_image_disable_native_cache, "NY_IMAGE_DISABLE_NATIVE") }
   _image_disable_native_cache == 1
}

fn _image_decode_dispatch(any data, str ext="") any {
   if !is_str(data) || data.len < 4 { return 0 }
   def b0, b1 = load8(data, 0), load8(data, 1)
   def b2, b3 = load8(data, 2), load8(data, 3)
   if b0 == 137 && b1 == 80 && b2 == 78 && b3 == 71 { return png.decode(data) }
   if b0 == 255 && b1 == 216 && b2 == 255 { return jpeg.decode(data) }
   if b0 == 66 && b1 == 77 { return bmp.decode(data) }
   if b0 == 71 && b1 == 73 && b2 == 70 && b3 == 56 { return gif.decode(data) }
   if data.len >= 12 && b0 == 82 && b1 == 73 && b2 == 70 && b3 == 70 &&
   load8(data, 8) == 87 && load8(data, 9) == 69 && load8(data, 10) == 66 && load8(data, 11) == 80{
      return webp.decode(data)
   }
   if b0 == 118 && b1 == 47 && b2 == 49 && b3 == 1 { return _decode_exr_staged(data, ext) }
   def svg_try = svg.decode(data, ext)
   if svg_try { return svg_try }
   def lext = str.lower(ext)
   if lext == ".tga" { return tga.decode(data) } elif lext == ".bmp" {
      return bmp.decode(data)
   } elif lext == ".png" {
      return png.decode(data)
   } elif lext == ".jpg" || lext == ".jpeg" {
      return jpeg.decode(data)
   } elif lext == ".gif" {
      return gif.decode(data)
   } elif lext == ".webp" {
      return webp.decode(data)
   } elif lext == ".exr" {
      return _decode_exr_staged(data, ext)
   } elif lext == ".svg" {
      return svg.decode(data)
   }
   if data.len > 18 {
      def img_type = load8(data, 2)
      if img_type == 2 || img_type == 3 || img_type == 10 || img_type == 11 { return tga.decode(data) }
   }
   0
}

fn decode(any data, str ext="") any {
   "Decodes an image from bytes. Optional `ext` can help with TGA/BMP dispatch."
   _image_decode_dispatch(data, ext)
}

fn load(str path, int _req_comp=0) any {
   "Loads and decodes an image file from `path`."
   def lpath = str.lower(path)
   if str.endswith(lpath, ".exr") {
      def exr_img = exr.load_path(path)
      if exr_img { return exr_img }
      _image_dbg("exr backend failed for " + path)
      return 0
   }
   if str.endswith(lpath, ".svg") {
      def svg_img = svg.load_path(path)
      if svg_img { return svg_img }
      _image_dbg("svg backend failed for " + path + ": " + svg.last_error())
      return 0
   }
   def res = file_read(path)
   if is_ok(res) {
      ;; Keep bytes string-typed across the generic Result unwrap ABI.
      def str bytes = unwrap(res)
      if _image_native_disabled() {
         _image_dbg("native disabled for " + path)
         return 0
      }
      mut any native = 0
      if _image_lock_native_decode() {
         def mu = _image_decode_mutex()
         if mu { mutex_lock(mu) }
         native = _image_decode_dispatch(bytes, path)
         if mu { mutex_unlock(mu) }
      } else {
         native = _image_decode_dispatch(bytes, path)
      }
      if native { return native }
      _image_dbg("native failed for " + path)
      return 0
   }
   0
}

fn encode(any img, str format="bmp") any {
   "Encodes an image dictionary into a byte string of the specified `format`."
   if eq(format, "bmp") { return bmp.encode(img) }
   if eq(format, "tga") { return tga.encode(img) }
   if eq(format, "png") { return png.encode(img) }
   if eq(format, "jpg") || eq(format, "jpeg") { return jpeg.encode(img) }
   if eq(format, "gif") { return gif.encode(img) }
   0
}

fn save(any img, str path, str format="auto") any {
   "Saves an image object to a file."
   mut fmt = format
   if eq(fmt, "auto") {
      if str.endswith(path, ".bmp") { fmt = "bmp" }
      elif str.endswith(path, ".tga") { fmt = "tga" }
      elif str.endswith(path, ".png") { fmt = "png" }
      elif str.endswith(path, ".jpg") || str.endswith(path, ".jpeg") { fmt = "jpeg" }
      elif str.endswith(path, ".gif") { fmt = "gif" }
      else { fmt = "bmp" }
   }
   if eq(fmt, "tga") { return tga.save(img, path) }
   def data = encode(img, fmt)
   if data { return file_write(path, data) }
   0
}

fn free(any img) any {
   "Decoded image payloads are stored as Ny strings via init_str(...).
   Manual freeing here corrupts ownership because callers still hold dict refs
   and the runtime will release the string storage on its own."
   if !img || !is_dict(img) { return nil }
   nil
}

fn get_info(any img) any {
   "Returns basic info from a loaded image dict."
   if !is_dict(img) { return 0 }
   mut info = dict(4)
   info = info.set("width",  img.get("width", 0))
   info = info.set("height", img.get("height", 0))
   info = info.set("bpp",    img.get("bpp", 0))
   info
}
