;; Operations on packed, 8-bit image dictionaries returned by img.decode.
;; Results own a new Ny string and never modify their input.
module core.math.parse.img.ops(create, crop, resize, dither, canvas, flip, rotate90, channel, invert, checked, grayscale, tint, brightness, contrast, replace_color, alpha_premultiply, alpha_mask, alpha_crop, alpha_clear, alpha_bounds, rgba_colors, palette, draw_pixel, draw_rect, draw_line, draw_circle, draw_rect_outline, draw_circle_outline, draw_triangle, draw_triangle_outline, draw_triangle_gradient, draw_rect_gradient, draw_circle_gradient, draw_image, draw_image_scaled, draw_image_ex, draw_text, gradient_linear, gradient_radial, gradient_square, rotate, convolve, blur, white_noise, perlin_noise, cellular, convert_channels, to_pot, mipmaps, pack16, unpack16)
use core
use core.math (floor, abs, sin, cos, sqrt)
use core.os.ui.font.truetype as ttf

fn _valid(any image) bool {
   if !is_dict(image) { return false }
   def int w = int(image.get("width", 0))
   def int h = int(image.get("height", 0))
   def int c = int(image.get("channels", 0))
   def data = image.get("data", nil)
   if w <= 0 || h <= 0 || c < 1 || c > 4 || !is_str(data) { return false }
   ;; Bound multiplication before calculating the required byte count.
   if w > 1000000 || h > 1000000 || w * h > 268435455 / c { return false }
   data.len >= w * h * c
}

fn create(int width, int height, list color) any {
   "Creates an image filled with one to four byte channels, or returns 0 for invalid dimensions or color."
   def int channels = color.len
   if width <= 0 || height <= 0 || channels < 1 || channels > 4 { return 0 }
   if width > 1000000 || height > 1000000 || width * height > 268435455 / channels { return 0 }
   def int size = width * height * channels
   def raw = malloc(size + 32)
   if !raw { return 0 }
   def data = init_str(raw, size)
   mut int i = 0
   while i < size {
      def int v = int(color.get(i % channels, 0))
      store8(data, v < 0 ? 0 : (v > 255 ? 255 : v), i)
      i += 1
   }
   {"width": width, "height": height, "channels": channels, "data": data}
}

fn convert_channels(any image, int target_channels) any {
   "Converts packed 8-bit pixels between grayscale, gray-alpha, RGB and RGBA channel layouts."
   if !_valid(image) || target_channels < 1 || target_channels > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int sc = int(image.get("channels"))
   if sc == target_channels { return crop(image, 0, 0, w, h) }
   def out = create(w, h, target_channels == 1 ? [0] : (target_channels == 2 ? [0, 0] : (target_channels == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int i = 0
   while i < w * h {
      def int sp = i * sc
      def int dp = i * target_channels
      mut int r = 0
      mut int g = 0
      mut int b = 0
      mut int a = 255
      if sc <= 2 { r = int(load8(src, sp)) g = r b = r if sc == 2 { a = int(load8(src, sp + 1)) } }
      else { r = int(load8(src, sp)) g = int(load8(src, sp + 1)) b = int(load8(src, sp + 2)) if sc == 4 { a = int(load8(src, sp + 3)) } }
      if target_channels <= 2 {
         def int gray = (r * 299 + g * 587 + b * 114 + 500) / 1000
         store8(dst, gray, dp)
         if target_channels == 2 { store8(dst, a, dp + 1) }
      } else {
         store8(dst, r, dp) store8(dst, g, dp + 1) store8(dst, b, dp + 2)
         if target_channels == 4 { store8(dst, a, dp + 3) }
      }
      i += 1
   }
   out
}

fn _pot(int value) int {
   mut int result = 1
   while result < value && result < 1048576 { result *= 2 }
   result
}

fn to_pot(any image, list fill) any {
   "Pads an image at the top-left to the next power-of-two dimensions using a channel-matched fill."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   if fill.len != c || w > 1048576 || h > 1048576 { return 0 }
   def int pw = _pot(w)
   def int ph = _pot(h)
   if pw < w || ph < h { return 0 }
   canvas(image, pw, ph, 0, 0, fill)
}

fn mipmaps(any image) any {
   "Returns a list of independent image levels, including the base image and bilinear half-size levels to 1x1."
   if !_valid(image) { return 0 }
   mut levels = [crop(image, 0, 0, int(image.get("width")), int(image.get("height")))]
   mut any current = levels.get(0)
   mut int w = int(current.get("width"))
   mut int h = int(current.get("height"))
   while w > 1 || h > 1 {
      def int nw = w > 1 ? w / 2 : 1
      def int nh = h > 1 ? h / 2 : 1
      current = resize(current, nw, nh, "bilinear")
      if !current { return 0 }
      levels = levels.append(current)
      w = nw h = nh
   }
   levels
}

fn pack16(any image, str format) any {
   "Packs an 8-bit image to little-endian RGB565, RGBA5551 or RGBA4444 pixel words."
   if !_valid(image) || (format != "rgb565" && format != "rgba5551" && format != "rgba4444") { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def rgba = convert_channels(image, 4)
   if !rgba { return 0 }
   def src = rgba.get("data")
   def raw = malloc(w * h * 2 + 32)
   if !raw { return 0 }
   def data = init_str(raw, w * h * 2)
   mut int i = 0
   while i < w * h {
      def int p = i * 4
      def int r = int(load8(src, p))
      def int g = int(load8(src, p + 1))
      def int b = int(load8(src, p + 2))
      def int a = int(load8(src, p + 3))
      mut int packed = 0
      if format == "rgb565" {
         packed = (((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) | ((b * 31 + 127) / 255)
      } elif format == "rgba5551" {
         packed = (((r * 31 + 127) / 255) << 11) | (((g * 31 + 127) / 255) << 6) | (((b * 31 + 127) / 255) << 1) | (a >= 128 ? 1 : 0)
      } else {
         packed = (((r * 15 + 127) / 255) << 12) | (((g * 15 + 127) / 255) << 8) | (((b * 15 + 127) / 255) << 4) | ((a * 15 + 127) / 255)
      }
      store8(data, packed & 255, i * 2)
      store8(data, (packed >> 8) & 255, i * 2 + 1)
      i += 1
   }
   {"width": w, "height": h, "format": format, "bits_per_pixel": 16, "data": data}
}

fn unpack16(any packed_image) any {
   "Unpacks a little-endian RGB565, RGBA5551 or RGBA4444 image to RGBA8."
   if !is_dict(packed_image) { return 0 }
   def int w = int(packed_image.get("width", 0))
   def int h = int(packed_image.get("height", 0))
   def format = packed_image.get("format", "")
   def data = packed_image.get("data", nil)
   if w <= 0 || h <= 0 || w > 1000000 || h > 1000000 || w * h > 134217727 || !is_str(data) || data.len < w * h * 2 || (format != "rgb565" && format != "rgba5551" && format != "rgba4444") { return 0 }
   def out = create(w, h, [0, 0, 0, 255])
   if !out { return 0 }
   def dst = out.get("data")
   mut int i = 0
   while i < w * h {
      def int packed = int(load8(data, i * 2)) | (int(load8(data, i * 2 + 1)) << 8)
      mut int r = 0
      mut int g = 0
      mut int b = 0
      mut int a = 255
      if format == "rgb565" {
         r = ((packed >> 11) & 31) * 255 / 31
         g = ((packed >> 5) & 63) * 255 / 63
         b = (packed & 31) * 255 / 31
      } elif format == "rgba5551" {
         r = ((packed >> 11) & 31) * 255 / 31
         g = ((packed >> 6) & 31) * 255 / 31
         b = ((packed >> 1) & 31) * 255 / 31
         a = (packed & 1) != 0 ? 255 : 0
      } else {
         r = ((packed >> 12) & 15) * 17
         g = ((packed >> 8) & 15) * 17
         b = ((packed >> 4) & 15) * 17
         a = (packed & 15) * 17
      }
      store8(dst, r, i * 4) store8(dst, g, i * 4 + 1) store8(dst, b, i * 4 + 2) store8(dst, a, i * 4 + 3)
      i += 1
   }
   out
}

fn crop(any image, int x, int y, int width, int height) any {
   "Copies a rectangle within the image. Out-of-bounds or empty rectangles return 0."
   if !_valid(image) { return 0 }
   def int sw = int(image.get("width"))
   def int sh = int(image.get("height"))
   def int c = int(image.get("channels"))
   if x < 0 || y < 0 || width <= 0 || height <= 0 || x >= sw || y >= sh || width > sw - x || height > sh - y { return 0 }
   def out = create(width, height, c == 1 ? [0] : (c == 2 ? [0, 0] : (c == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int row = 0
   while row < height {
      mut int col = 0
      while col < width * c {
         store8(dst, load8(src, ((y + row) * sw + x) * c + col), row * width * c + col)
         col += 1
      }
      row += 1
   }
   out
}

fn canvas(any image, int width, int height, int x, int y, list color) any {
   "Copies an image onto a new canvas at(x, y), clipping it to the canvas."
   if !_valid(image) { return 0 }
   def int sw = int(image.get("width"))
   def int sh = int(image.get("height"))
   def int c = int(image.get("channels"))
   if color.len != c { return 0 }
   def out = create(width, height, color)
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int sy = 0
   while sy < sh {
      def int dy = sy + y
      if dy >= 0 && dy < height {
         mut int sx = 0
         while sx < sw {
            def int dx = sx + x
            if dx >= 0 && dx < width {
               mut int k = 0
               while k < c { store8(dst, load8(src, (sy * sw + sx) * c + k), (dy * width + dx) * c + k) k += 1 }
            }
            sx += 1
         }
      }
      sy += 1
   }
   out
}

fn flip(any image, bool horizontal, bool vertical) any {
   "Returns a horizontally and/or vertically mirrored copy."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int y = 0
   while y < h {
      mut int x = 0
      while x < w {
         def int sx = horizontal ? w - x - 1 : x
         def int sy = vertical ? h - y - 1 : y
         mut int k = 0
         while k < c { store8(dst, load8(src, (sy * w + sx) * c + k), (y * w + x) * c + k) k += 1 }
         x += 1
      }
      y += 1
   }
   out
}

fn rotate90(any image, bool clockwise=true) any {
   "Returns a copy rotated by 90 degrees."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = create(h, w, c == 1 ? [0] : (c == 2 ? [0, 0] : (c == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int y = 0
   while y < h {
      mut int x = 0
      while x < w {
         def int dx = clockwise ? h - y - 1 : y
         def int dy = clockwise ? x : w - x - 1
         mut int k = 0
         while k < c { store8(dst, load8(src, (y * w + x) * c + k), (dy * h + dx) * c + k) k += 1 }
         x += 1
      }
      y += 1
   }
   out
}

fn channel(any image, int selected) any {
   "Extracts a zero-based channel as a one-channel image."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   if selected < 0 || selected >= c { return 0 }
   def out = create(w, h, [0])
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int i = 0
   while i < w * h { store8(dst, load8(src, i * c + selected), i) i += 1 }
   out
}

fn invert(any image) any {
   "Returns a color-inverted copy, preserving alpha if present."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   def int colors = (c == 2 || c == 4) ? c - 1 : c
   mut int i = 0
   while i < w * h {
      mut int k = 0
      while k < colors { def int p = i * c + k store8(data, 255 - load8(data, p), p) k += 1 }
      i += 1
   }
   out
}

fn grayscale(any image) any {
   "Converts RGB or RGBA pixels to luminance while preserving alpha and channel count."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   if c < 3 { return out }
   def data = out.get("data")
   mut int i = 0
   while i < w * h {
      def int p = i * c
      def int y = (int(load8(data, p)) * 299 + int(load8(data, p + 1)) * 587 + int(load8(data, p + 2)) * 114 + 500) / 1000
      store8(data, y, p) store8(data, y, p + 1) store8(data, y, p + 2)
      i += 1
   }
   out
}

fn tint(any image, list color) any {
   "Multiplies each pixel channel by an 8-bit tint. The tint must have one value per image channel."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   if color.len != c { return 0 }
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   mut int i = 0
   while i < w * h {
      mut int k = 0
      while k < c {
         def int p = i * c + k
         def int t = int(color.get(k))
         if t < 0 { t = 0 } elif t > 255 { t = 255 }
         store8(data, (int(load8(data, p)) * t + 127) / 255, p)
         k += 1
      }
      i += 1
   }
   out
}

fn brightness(any image, int amount) any {
   "Adds an amount from -255 to 255 to color channels, preserving alpha."
   if !_valid(image) || amount < -255 || amount > 255 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int colors = (c == 2 || c == 4) ? c - 1 : c
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   mut int i = 0
   while i < w * h {
      mut int k = 0
      while k < colors {
         def int p = i * c + k
         def int v = int(load8(data, p)) + amount
         store8(data, v < 0 ? 0 : (v > 255 ? 255 : v), p)
         k += 1
      }
      i += 1
   }
   out
}

fn contrast(any image, int amount) any {
   "Applies raylib's contrast range(-100..100), preserving alpha."
   if !_valid(image) || amount < -100 || amount > 100 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int colors = (c == 2 || c == 4) ? c - 1 : c
   def f64 f = (259.0 * (float(amount) + 255.0)) / (255.0 * (259.0 - float(amount)))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   mut int i = 0
   while i < w * h {
      mut int k = 0
      while k < colors {
         def int p = i * c + k
         def f64 raw = f * (float(load8(data, p)) - 128.0) + 128.0
         def int v = int(raw + 0.5)
         store8(data, v < 0 ? 0 : (v > 255 ? 255 : v), p)
         k += 1
      }
      i += 1
   }
   out
}

fn replace_color(any image, list from, list to) any {
   "Replaces pixels exactly equal to `from` with `to`; both colors match image channels."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   if from.len != c || to.len != c { return 0 }
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   mut int i = 0
   while i < w * h {
      mut bool matches = true
      mut int k = 0
      while k < c { if int(load8(data, i * c + k)) != int(from.get(k)) { matches = false } k += 1 }
      if matches {
         k = 0
         while k < c {
            def int v = int(to.get(k))
            store8(data, v < 0 ? 0 : (v > 255 ? 255 : v), i * c + k)
            k += 1
         }
      }
      i += 1
   }
   out
}

fn alpha_premultiply(any image) any {
   "Premultiplies color channels by alpha for two- or four-channel images."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   if c != 2 && c != 4 { return out }
   def data = out.get("data")
   mut int i = 0
   while i < w * h {
      def int p = i * c
      def int a = int(load8(data, p + c - 1))
      mut int k = 0
      while k < c - 1 { store8(data, (int(load8(data, p + k)) * a + 127) / 255, p + k) k += 1 }
      i += 1
   }
   out
}

fn alpha_mask(any image, any mask) any {
   "Applies a one-channel or two-channel mask of matching size to image alpha(or appends alpha to RGB)."
   if !_valid(image) || !_valid(mask) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int mw = int(mask.get("width"))
   def int mh = int(mask.get("height"))
   def int mc = int(mask.get("channels"))
   if w != mw || h != mh || (mc != 1 && mc != 2) || (c != 1 && c != 2 && c != 3 && c != 4) { return 0 }
   def int oc = (c == 1 || c == 3) ? c + 1 : c
   def out = create(w, h, oc == 2 ? [0, 0] : (oc == 4 ? [0, 0, 0, 0] : [0]))
   if !out { return 0 }
   def src = image.get("data")
   def md = mask.get("data")
   def dst = out.get("data")
   mut int i = 0
   while i < w * h {
      mut int k = 0
      while k < c { store8(dst, load8(src, i * c + k), i * oc + k) k += 1 }
      def int ma = mc == 2 ? int(load8(md, i * mc + 1)) : int(load8(md, i * mc))
      def int original = (c == 2 || c == 4) ? int(load8(src, i * c + c - 1)) : 255
      store8(dst, (original * ma + 127) / 255, i * oc + oc - 1)
      i += 1
   }
   out
}

fn alpha_bounds(any image, f64 threshold=0.0) any {
   "Returns the visible alpha bounding rectangle as x, y, width, height; empty images return a zero rectangle."
   if !_valid(image) || threshold < 0.0 || threshold > 1.0 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def data = image.get("data")
   def int alpha_channel = (c == 2 || c == 4) ? c - 1 : -1
   def int t = int(threshold * 255.0)
   mut int xmin = w
   mut int ymin = h
   mut int xmax = -1
   mut int ymax = -1
   mut int y = 0
   while y < h {
      mut int x = 0
      while x < w {
         def int a = alpha_channel < 0 ? 255 : int(load8(data, (y * w + x) * c + alpha_channel))
         if a > t {
            if x < xmin { xmin = x }
            if y < ymin { ymin = y }
            if x > xmax { xmax = x }
            if y > ymax { ymax = y }
         }
         x += 1
      }
      y += 1
   }
   if xmax < xmin || ymax < ymin { return {"x": 0, "y": 0, "width": 0, "height": 0} }
   {"x": xmin, "y": ymin, "width": xmax - xmin + 1, "height": ymax - ymin + 1}
}

fn alpha_crop(any image, f64 threshold=0.0) any {
   "Crops to the bounding box of pixels with alpha above the 0..1 threshold."
   def bounds = alpha_bounds(image, threshold)
   if !bounds { return 0 }
   def int w = int(bounds.get("width", 0))
   def int h = int(bounds.get("height", 0))
   if w <= 0 || h <= 0 { return crop(image, 0, 0, int(image.get("width")), int(image.get("height"))) }
   crop(image, int(bounds.get("x")), int(bounds.get("y")), w, h)
}

fn alpha_clear(any image, list color, f64 threshold) any {
   "Replaces pixels with alpha at or below the 0..1 threshold by an RGB(A) color."
   if !_valid(image) || threshold < 0.0 || threshold > 1.0 || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   if c != 2 && c != 4 { return out }
   def data = out.get("data")
   def int t = int(threshold * 255.0)
   def int r = _rgba(color, 0)
   def int g = _rgba(color, 1)
   def int b = _rgba(color, 2)
   def int a = color.len == 4 ? _rgba(color, 3) : 255
   def int gray = (r * 299 + g * 587 + b * 114 + 500) / 1000
   mut int i = 0
   while i < w * h {
      def int p = i * c
      if int(load8(data, p + c - 1)) <= t {
         if c == 2 { store8(data, gray, p) store8(data, a, p + 1) }
         else { store8(data, r, p) store8(data, g, p + 1) store8(data, b, p + 2) store8(data, a, p + 3) }
      }
      i += 1
   }
   out
}

fn pixel(any image, int x, int y) any {
   "Returns the packed pixel channels at x,y as a list, or 0 when outside the image."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   if x < 0 || y < 0 || x >= w || y >= h { return 0 }
   def data = image.get("data")
   mut out = []
   mut int k = 0
   while k < c { out = out.append(load8(data, (y * w + x) * c + k)) k += 1 }
   out
}

fn rgba_colors(any image) any {
   "Returns all pixels in row-major order as RGBA byte lists, expanding grayscale and adding opaque alpha when absent."
   if !_valid(image) { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def data = image.get("data")
   mut out = []
   mut int i = 0
   while i < w * h {
      def int p = i * c
      mut int r = 0
      mut int g = 0
      mut int b = 0
      mut int a = 255
      if c == 1 || c == 2 { r = int(load8(data, p)) g = r b = r if c == 2 { a = int(load8(data, p + 1)) } }
      else { r = int(load8(data, p)) g = int(load8(data, p + 1)) b = int(load8(data, p + 2)) if c == 4 { a = int(load8(data, p + 3)) } }
      out = out.append([r, g, b, a])
      i += 1
   }
   out
}

fn palette(any image, int max_colors) any {
   "Returns unique visible RGBA byte lists in first-seen order, capped at max_colors."
   if !_valid(image) || max_colors <= 0 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def data = image.get("data")
   mut out = []
   mut int i = 0
   while i < w * h && out.len < max_colors {
      def int p = i * c
      mut int r = 0
      mut int g = 0
      mut int b = 0
      mut int a = 255
      if c == 1 || c == 2 { r = int(load8(data, p)) g = r b = r if c == 2 { a = int(load8(data, p + 1)) } }
      else { r = int(load8(data, p)) g = int(load8(data, p + 1)) b = int(load8(data, p + 2)) if c == 4 { a = int(load8(data, p + 3)) } }
      if a > 0 {
         mut bool found = false
         mut int j = 0
         while j < out.len {
            def entry = out.get(j)
            if int(entry.get(0)) == r && int(entry.get(1)) == g && int(entry.get(2)) == b && int(entry.get(3)) == a { found = true }
            j += 1
         }
         if !found { out = out.append([r, g, b, a]) }
      }
      i += 1
   }
   out
}

fn _draw_color(any data, int p, int channels, list color) any {
   def int r = int(color.get(0, 0))
   def int g = int(color.get(1, r))
   def int b = int(color.get(2, r))
   def int a = int(color.get(3, 255))
   if r < 0 { r = 0 } elif r > 255 { r = 255 }
   if g < 0 { g = 0 } elif g > 255 { g = 255 }
   if b < 0 { b = 0 } elif b > 255 { b = 255 }
   if a < 0 { a = 0 } elif a > 255 { a = 255 }
   if channels == 1 || channels == 2 {
      def int gray = (r * 299 + g * 587 + b * 114 + 500) / 1000
      store8(data, gray, p)
      if channels == 2 { store8(data, a, p + 1) }
   } else {
      store8(data, r, p) store8(data, g, p + 1) store8(data, b, p + 2)
      if channels == 4 { store8(data, a, p + 3) }
   }
}

fn draw_pixel(any image, int x, int y, list color) any {
   "Returns a copy with one in-bounds pixel replaced by RGB/RGBA color."
   if !_valid(image) || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   if x >= 0 && y >= 0 && x < w && y < h { _draw_color(out.get("data"), (y * w + x) * c, c, color) }
   out
}

fn draw_rect(any image, int x, int y, int width, int height, list color) any {
   "Returns a copy with a clipped, color-filled rectangle drawn."
   if !_valid(image) || width <= 0 || height <= 0 || width > 10000000 || height > 10000000 || x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000 || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def int x0 = x < 0 ? 0 : x
   def int y0 = y < 0 ? 0 : y
   def int x1 = x + width > w ? w : x + width
   def int y1 = y + height > h ? h : y + height
   if x0 >= x1 || y0 >= y1 { return out }
   def data = out.get("data")
   mut int py = y0
   while py < y1 {
      mut int px = x0
      while px < x1 { _draw_color(data, (py * w + px) * c, c, color) px += 1 }
      py += 1
   }
   out
}

fn _line_code(f64 x, f64 y, int width, int height) int {
   mut int code = 0
   if x < 0.0 { code = code | 1 } elif x >= float(width) { code = code | 2 }
   if y < 0.0 { code = code | 4 } elif y >= float(height) { code = code | 8 }
   code
}

fn draw_line(any image, int x0, int y0, int x1, int y1, list color) any {
   "Returns a copy with a clipped integer line drawn using Bresenham's algorithm."
   if !_valid(image) || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   mut f64 ax = float(x0)
   mut f64 ay = float(y0)
   mut f64 bx = float(x1)
   mut f64 by = float(y1)
   mut int ca = _line_code(ax, ay, w, h)
   mut int cb = _line_code(bx, by, w, h)
   mut bool accept = false
   mut int rounds = 0
   while rounds < 16 {
      if ca == 0 && cb == 0 { accept = true rounds = 16 }
      elif (ca & cb) != 0 { rounds = 16 }
      else {
         def int co = ca != 0 ? ca : cb
         mut f64 ix = 0.0
         mut f64 iy = 0.0
         if (co & 8) != 0 { iy = float(h - 1) ix = ax + (bx - ax) * (iy - ay) / (by - ay) }
         elif (co & 4) != 0 { iy = 0.0 ix = ax + (bx - ax) * (iy - ay) / (by - ay) }
         elif (co & 2) != 0 { ix = float(w - 1) iy = ay + (by - ay) * (ix - ax) / (bx - ax) }
         else { ix = 0.0 iy = ay + (by - ay) * (ix - ax) / (bx - ax) }
         if co == ca { ax = ix ay = iy ca = _line_code(ax, ay, w, h) }
         else { bx = ix by = iy cb = _line_code(bx, by, w, h) }
         rounds += 1
      }
   }
   if !accept { return out }
   mut int px0 = int(ax + 0.5)
   mut int py0 = int(ay + 0.5)
   def int px1 = int(bx + 0.5)
   def int py1 = int(by + 0.5)
   def int dx0 = px1 >= px0 ? px1 - px0 : px0 - px1
   def int dy0 = py1 >= py0 ? py1 - py0 : py0 - py1
   def int sx = px0 < px1 ? 1 : -1
   def int sy = py0 < py1 ? 1 : -1
   mut int err = dx0 - dy0
   def data = out.get("data")
   mut bool done = false
   while !done {
      _draw_color(data, (py0 * w + px0) * c, c, color)
      if px0 == px1 && py0 == py1 { done = true }
      else {
         def int twice = err * 2
         if twice > -dy0 { err -= dy0 px0 += sx }
         if twice < dx0 { err += dx0 py0 += sy }
      }
   }
   out
}

fn draw_circle(any image, int center_x, int center_y, int radius, list color) any {
   "Returns a copy with a filled circle drawn, clipped to the image."
   if !_valid(image) || radius < 0 || radius > 1000000 || center_x < -2000000 || center_x > 2000000 || center_y < -2000000 || center_y > 2000000 || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def int top = center_y - radius < 0 ? 0 : center_y - radius
   def int bottom = center_y + radius >= h ? h - 1 : center_y + radius
   if top > bottom { return out }
   def data = out.get("data")
   mut int y = top
   while y <= bottom {
      def int dy = y - center_y
      def f64 rem = float(radius * radius - dy * dy)
      def int span = int(sqrt(rem))
      def int left = center_x - span < 0 ? 0 : center_x - span
      def int right = center_x + span >= w ? w - 1 : center_x + span
      if left <= right {
         mut int x = left
         while x <= right { _draw_color(data, (y * w + x) * c, c, color) x += 1 }
      }
      y += 1
   }
   out
}

fn draw_rect_outline(any image, int x, int y, int width, int height, list color) any {
   "Returns a copy with a clipped one-pixel rectangle outline."
   if !_valid(image) || width <= 0 || height <= 0 || width > 10000000 || height > 10000000 || x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000 || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def int x0 = x < 0 ? 0 : x
   def int y0 = y < 0 ? 0 : y
   def int x1 = x + width - 1 >= w ? w - 1 : x + width - 1
   def int y1 = y + height - 1 >= h ? h - 1 : y + height - 1
   if x0 > x1 || y0 > y1 { return out }
   def data = out.get("data")
   mut int px = x0
   while px <= x1 {
      _draw_color(data, (y0 * w + px) * c, c, color)
      if y1 != y0 { _draw_color(data, (y1 * w + px) * c, c, color) }
      px += 1
   }
   mut int py = y0 + 1
   while py < y1 {
      _draw_color(data, (py * w + x0) * c, c, color)
      if x1 != x0 { _draw_color(data, (py * w + x1) * c, c, color) }
      py += 1
   }
   out
}

fn draw_circle_outline(any image, int center_x, int center_y, int radius, list color) any {
   "Returns a copy with a clipped one-pixel circle outline."
   if !_valid(image) || radius < 0 || radius > 1000000 || center_x < -2000000 || center_x > 2000000 || center_y < -2000000 || center_y > 2000000 || color.len < 3 || color.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   mut int dy = -radius
   while dy <= radius {
      def int dx = int(sqrt(float(radius * radius - dy * dy)))
      def int y = center_y + dy
      def int xl = center_x - dx
      def int xr = center_x + dx
      if y >= 0 && y < h {
         if xl >= 0 && xl < w { _draw_color(data, (y * w + xl) * c, c, color) }
         if xr >= 0 && xr < w && xr != xl { _draw_color(data, (y * w + xr) * c, c, color) }
      }
      dy += 1
   }
   out
}

fn draw_triangle(any image, int x1, int y1, int x2, int y2, int x3, int y3, list color) any {
   "Returns a copy with a clipped, solid triangle rasterized using signed edge tests."
   if !_valid(image) || color.len < 3 || color.len > 4 || x1 < -10000000 || x1 > 10000000 || x2 < -10000000 || x2 > 10000000 || x3 < -10000000 || x3 > 10000000 || y1 < -10000000 || y1 > 10000000 || y2 < -10000000 || y2 > 10000000 || y3 < -10000000 || y3 > 10000000 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def f64 area = float(x2 - x1) * float(y3 - y1) - float(x3 - x1) * float(y2 - y1)
   if area == 0.0 {
      def first = draw_line(out, x1, y1, x2, y2, color)
      def second = draw_line(first, x2, y2, x3, y3, color)
      return draw_line(second, x3, y3, x1, y1, color)
   }
   def int min_x = (x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3))
   def int max_x = (x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3))
   def int min_y = (y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3))
   def int max_y = (y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3))
   def int left = min_x < 0 ? 0 : min_x
   def int right = max_x >= w ? w - 1 : max_x
   def int top = min_y < 0 ? 0 : min_y
   def int bottom = max_y >= h ? h - 1 : max_y
   if left > right || top > bottom { return out }
   def data = out.get("data")
   mut int y = top
   while y <= bottom {
      mut int x = left
      while x <= right {
         def f64 e1 = float(x2 - x1) * float(y - y1) - float(y2 - y1) * float(x - x1)
         def f64 e2 = float(x3 - x2) * float(y - y2) - float(y3 - y2) * float(x - x2)
         def f64 e3 = float(x1 - x3) * float(y - y3) - float(y1 - y3) * float(x - x3)
         if (area > 0.0 && e1 >= 0.0 && e2 >= 0.0 && e3 >= 0.0) || (area < 0.0 && e1 <= 0.0 && e2 <= 0.0 && e3 <= 0.0) {
            _draw_color(data, (y * w + x) * c, c, color)
         }
         x += 1
      }
      y += 1
   }
   out
}

fn draw_triangle_outline(any image, int x1, int y1, int x2, int y2, int x3, int y3, list color) any {
   "Returns a copy with the three clipped triangle edges drawn."
   if !_valid(image) || color.len < 3 || color.len > 4 { return 0 }
   def first = draw_line(image, x1, y1, x2, y2, color)
   def second = draw_line(first, x2, y2, x3, y3, color)
   draw_line(second, x3, y3, x1, y1, color)
}

fn draw_triangle_gradient(any image, int x1, int y1, list color1, int x2, int y2, list color2, int x3, int y3, list color3) any {
   "Returns a copy with barycentrically interpolated RGB(A) triangle colors."
   if !_valid(image) || color1.len < 3 || color1.len > 4 || color2.len < 3 || color2.len > 4 || color3.len < 3 || color3.len > 4 || x1 < -10000000 || x1 > 10000000 || x2 < -10000000 || x2 > 10000000 || x3 < -10000000 || x3 > 10000000 || y1 < -10000000 || y1 > 10000000 || y2 < -10000000 || y2 > 10000000 || y3 < -10000000 || y3 > 10000000 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def f64 area = float(x2 - x1) * float(y3 - y1) - float(x3 - x1) * float(y2 - y1)
   if area == 0.0 { return out }
   def int left0 = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3)
   def int right0 = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3)
   def int top0 = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3)
   def int bottom0 = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3)
   def int left = left0 < 0 ? 0 : left0
   def int right = right0 >= w ? w - 1 : right0
   def int top = top0 < 0 ? 0 : top0
   def int bottom = bottom0 >= h ? h - 1 : bottom0
   if left > right || top > bottom { return out }
   def data = out.get("data")
   mut int y = top
   while y <= bottom {
      mut int x = left
      while x <= right {
         def f64 e1 = float(x2 - x1) * float(y - y1) - float(y2 - y1) * float(x - x1)
         def f64 e2 = float(x3 - x2) * float(y - y2) - float(y3 - y2) * float(x - x2)
         def f64 e3 = float(x1 - x3) * float(y - y3) - float(y1 - y3) * float(x - x3)
         if (area > 0.0 && e1 >= 0.0 && e2 >= 0.0 && e3 >= 0.0) || (area < 0.0 && e1 <= 0.0 && e2 <= 0.0 && e3 <= 0.0) {
            def f64 wa = (float(x2 - x) * float(y3 - y) - float(y2 - y) * float(x3 - x)) / area
            def f64 wb = (float(x3 - x) * float(y1 - y) - float(y3 - y) * float(x1 - x)) / area
            def f64 wc = 1.0 - wa - wb
            mut rgba = []
            mut int k = 0
            while k < 4 {
               def int a = k == 3 && color1.len == 3 ? 255 : _rgba(color1, k)
               def int b = k == 3 && color2.len == 3 ? 255 : _rgba(color2, k)
               def int d = k == 3 && color3.len == 3 ? 255 : _rgba(color3, k)
               def int v = int(float(a) * wa + float(b) * wb + float(d) * wc + 0.5)
               rgba = rgba.append(v < 0 ? 0 : (v > 255 ? 255 : v))
               k += 1
            }
            _draw_color(data, (y * w + x) * c, c, rgba)
         }
         x += 1
      }
      y += 1
   }
   out
}

fn draw_rect_gradient(any image, f64 x, f64 y, f64 width, f64 height, list top_left, list bottom_left, list bottom_right, list top_right) any {
   "Returns a copy with a clipped four-corner bilinear color gradient."
   if !_valid(image) || width <= 0.0 || height <= 0.0 || width > 10000000.0 || height > 10000000.0 || x < -10000000.0 || x > 10000000.0 || y < -10000000.0 || y > 10000000.0 || top_left.len < 3 || top_left.len > 4 || bottom_left.len < 3 || bottom_left.len > 4 || bottom_right.len < 3 || bottom_right.len > 4 || top_right.len < 3 || top_right.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   mut int x0 = int(floor(x))
   mut int y0 = int(floor(y))
   mut int x1 = -int(floor(-(x + width)))
   mut int y1 = -int(floor(-(y + height)))
   if x0 < 0 { x0 = 0 }
   if y0 < 0 { y0 = 0 }
   if x1 > w { x1 = w }
   if y1 > h { y1 = h }
   if x0 >= x1 || y0 >= y1 { return out }
   def data = out.get("data")
   mut int py = y0
   while py < y1 {
      def f64 ty0 = (float(py) - y) / height
      def f64 ty = ty0 < 0.0 ? 0.0 : (ty0 > 1.0 ? 1.0 : ty0)
      mut int px = x0
      while px < x1 {
         def f64 tx0 = (float(px) - x) / width
         def f64 tx = tx0 < 0.0 ? 0.0 : (tx0 > 1.0 ? 1.0 : tx0)
         def weights = [(1.0 - tx) * (1.0 - ty), (1.0 - tx) * ty, tx * ty, tx * (1.0 - ty)]
         mut rgba = []
         mut int k = 0
         while k < 4 {
            def int v = int(float(_rgba(top_left, k)) * float(weights.get(0)) + float(_rgba(bottom_left, k)) * float(weights.get(1)) + float(_rgba(bottom_right, k)) * float(weights.get(2)) + float(_rgba(top_right, k)) * float(weights.get(3)))
            rgba = rgba.append(k == 3 && top_left.len == 3 && bottom_left.len == 3 && bottom_right.len == 3 && top_right.len == 3 ? 255 : (v < 0 ? 0 : (v > 255 ? 255 : v)))
            k += 1
         }
         _draw_color(data, (py * w + px) * c, c, rgba)
         px += 1
      }
      py += 1
   }
   out
}

fn draw_circle_gradient(any image, f64 center_x, f64 center_y, f64 radius, list inner, list outer) any {
   "Returns a copy with a clipped radial color gradient inside a circle."
   if !_valid(image) || radius <= 0.0 || radius > 1000000.0 || center_x < -2000000.0 || center_x > 2000000.0 || center_y < -2000000.0 || center_y > 2000000.0 || inner.len < 3 || inner.len > 4 || outer.len < 3 || outer.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   mut int x0 = int(floor(center_x - radius))
   mut int y0 = int(floor(center_y - radius))
   mut int x1 = -int(floor(-(center_x + radius)))
   mut int y1 = -int(floor(-(center_y + radius)))
   if x0 < 0 { x0 = 0 }
   if y0 < 0 { y0 = 0 }
   if x1 > w { x1 = w }
   if y1 > h { y1 = h }
   if x0 >= x1 || y0 >= y1 { return out }
   def data = out.get("data")
   mut int y = y0
   while y < y1 {
      mut int x = x0
      while x < x1 {
         def f64 dx = float(x) + 0.5 - center_x
         def f64 dy = float(y) + 0.5 - center_y
         def f64 d2 = dx * dx + dy * dy
         if d2 <= radius * radius {
            def f64 t = sqrt(d2) / radius
            mut rgba = []
            mut int k = 0
            while k < 4 {
               def int a = k == 3 && inner.len == 3 ? 255 : _rgba(inner, k)
               def int b = k == 3 && outer.len == 3 ? 255 : _rgba(outer, k)
               def int v = int(float(a) + float(b - a) * t)
               rgba = rgba.append(v < 0 ? 0 : (v > 255 ? 255 : v))
               k += 1
            }
            _draw_color(data, (y * w + x) * c, c, rgba)
         }
         x += 1
      }
      y += 1
   }
   out
}

fn _rgba_at(any data, int p, int c, int channel) int {
   if c == 1 || c == 2 { return int(load8(data, p)) }
   int(load8(data, p + channel))
}

fn _alpha_at(any data, int p, int c) int {
   if c == 2 { return int(load8(data, p + 1)) }
   if c == 4 { return int(load8(data, p + 3)) }
   255
}

fn _write_blended(any data, int p, int c, int r, int g, int b, int a) any {
   if c == 1 || c == 2 {
      store8(data, (r * 299 + g * 587 + b * 114 + 500) / 1000, p)
      if c == 2 { store8(data, a, p + 1) }
   } else {
      store8(data, r, p) store8(data, g, p + 1) store8(data, b, p + 2)
      if c == 4 { store8(data, a, p + 3) }
   }
}

fn _blit_pixel(any dst, int dp, int dc, any src, int sp, int sc, int tr, int tg, int tb, int ta) any {
   def int sr = (_rgba_at(src, sp, sc, 0) * (tr + 1)) >> 8
   def int sg = (_rgba_at(src, sp, sc, 1) * (tg + 1)) >> 8
   def int sb = (_rgba_at(src, sp, sc, 2) * (tb + 1)) >> 8
   def int sa = (_alpha_at(src, sp, sc) * (ta + 1)) >> 8
   if sa != 0 {
      def int da = _alpha_at(dst, dp, dc)
      mut int rr = sr
      mut int rg = sg
      mut int rb = sb
      mut int ra = sa
      if sa != 255 {
         def int alpha = sa + 1
         ra = (alpha * 256 + da * (256 - alpha)) >> 8
         if ra > 0 {
            rr = (((sr * alpha * 256 + _rgba_at(dst, dp, dc, 0) * da * (256 - alpha)) / ra) >> 8)
            rg = (((sg * alpha * 256 + _rgba_at(dst, dp, dc, 1) * da * (256 - alpha)) / ra) >> 8)
            rb = (((sb * alpha * 256 + _rgba_at(dst, dp, dc, 2) * da * (256 - alpha)) / ra) >> 8)
         }
      }
      _write_blended(dst, dp, dc, rr, rg, rb, ra)
   }
}

fn draw_image(any image, any source, int x, int y, list tint=[255, 255, 255, 255]) any {
   "Alpha-composites a clipped source image into a copy of the destination with an RGB(A) tint."
   if !_valid(image) || !_valid(source) || tint.len < 3 || tint.len > 4 || x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int sw = int(source.get("width"))
   def int sh = int(source.get("height"))
   def int sc = int(source.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def int sx0 = x < 0 ? -x : 0
   def int sy0 = y < 0 ? -y : 0
   def int dx0 = x < 0 ? 0 : x
   def int dy0 = y < 0 ? 0 : y
   def int rw0 = sw - sx0
   def int rh0 = sh - sy0
   def int rw = rw0 < w - dx0 ? rw0 : w - dx0
   def int rh = rh0 < h - dy0 ? rh0 : h - dy0
   if rw <= 0 || rh <= 0 { return out }
   def src = source.get("data")
   def dst = out.get("data")
   def int tr = _rgba(tint, 0)
   def int tg = _rgba(tint, 1)
   def int tb = _rgba(tint, 2)
   def int ta = tint.len == 4 ? _rgba(tint, 3) : 255
   mut int py = 0
   while py < rh {
      mut int px = 0
      while px < rw {
         def int sp = ((sy0 + py) * sw + sx0 + px) * sc
         def int dp = ((dy0 + py) * w + dx0 + px) * c
         _blit_pixel(dst, dp, c, src, sp, sc, tr, tg, tb, ta)
         px += 1
      }
      py += 1
   }
   out
}

fn draw_image_scaled(any image, any source, int x, int y, int width, int height, list tint=[255, 255, 255, 255]) any {
   "Alpha-composites a nearest-scaled source rectangle into a clipped image copy."
   if !_valid(image) || !_valid(source) || width <= 0 || height <= 0 || width > 10000000 || height > 10000000 || x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000 || tint.len < 3 || tint.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int sw = int(source.get("width"))
   def int sh = int(source.get("height"))
   def int sc = int(source.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def int u0 = x < 0 ? -x : 0
   def int v0 = y < 0 ? -y : 0
   def int u1 = x + width > w ? w - x : width
   def int v1 = y + height > h ? h - y : height
   if u0 >= u1 || v0 >= v1 { return out }
   def src = source.get("data")
   def dst = out.get("data")
   def int tr = _rgba(tint, 0)
   def int tg = _rgba(tint, 1)
   def int tb = _rgba(tint, 2)
   def int ta = tint.len == 4 ? _rgba(tint, 3) : 255
   mut int v = v0
   while v < v1 {
      def int sy = v * sh / height
      mut int u = u0
      while u < u1 {
         def int sx = u * sw / width
         def int sp = (sy * sw + sx) * sc
         def int dp = ((y + v) * w + x + u) * c
         _blit_pixel(dst, dp, c, src, sp, sc, tr, tg, tb, ta)
         u += 1
      }
      v += 1
   }
   out
}

fn draw_image_ex(any image, any source, int x, int y, f64 rotation, f64 scale=1.0, list tint=[255, 255, 255, 255]) any {
   "Alpha-composites a clipped nearest-sampled source rotated around its top-left origin."
   if !_valid(image) || !_valid(source) || scale <= 0.0 || scale > 10000.0 || x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000 || tint.len < 3 || tint.len > 4 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int sw = int(source.get("width"))
   def int sh = int(source.get("height"))
   def int sc = int(source.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def f64 rad = rotation * 3.141592653589793 / 180.0
   def f64 cs = cos(rad)
   def f64 sn = sin(rad)
   def f64 x0 = float(x)
   def f64 y0 = float(y)
   def f64 x1 = x0 + float(sw) * scale * cs
   def f64 y1 = y0 + float(sw) * scale * sn
   def f64 x2 = x0 - float(sh) * scale * sn
   def f64 y2 = y0 + float(sh) * scale * cs
   def f64 x3 = x1 + x2 - x0
   def f64 y3 = y1 + y2 - y0
   def f64 minx0 = x0 < x1 ? (x0 < x2 ? (x0 < x3 ? x0 : x3) : (x2 < x3 ? x2 : x3)) : (x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3))
   def f64 maxx0 = x0 > x1 ? (x0 > x2 ? (x0 > x3 ? x0 : x3) : (x2 > x3 ? x2 : x3)) : (x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3))
   def f64 miny0 = y0 < y1 ? (y0 < y2 ? (y0 < y3 ? y0 : y3) : (y2 < y3 ? y2 : y3)) : (y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3))
   def f64 maxy0 = y0 > y1 ? (y0 > y2 ? (y0 > y3 ? y0 : y3) : (y2 > y3 ? y2 : y3)) : (y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3))
   mut int left = int(floor(minx0))
   mut int top = int(floor(miny0))
   mut int right = -int(floor(-maxx0))
   mut int bottom = -int(floor(-maxy0))
   if left < 0 { left = 0 }
   if top < 0 { top = 0 }
   if right > w { right = w }
   if bottom > h { bottom = h }
   if left >= right || top >= bottom { return out }
   def src = source.get("data")
   def dst = out.get("data")
   def int tr = _rgba(tint, 0)
   def int tg = _rgba(tint, 1)
   def int tb = _rgba(tint, 2)
   def int ta = tint.len == 4 ? _rgba(tint, 3) : 255
   mut int py = top
   while py < bottom {
      mut int px = left
      while px < right {
         def f64 dx = float(px) + 0.5 - float(x)
         def f64 dy = float(py) + 0.5 - float(y)
         def f64 sx0 = (dx * cs + dy * sn) / scale
         def f64 sy0 = (-dx * sn + dy * cs) / scale
         if sx0 >= 0.0 && sy0 >= 0.0 && sx0 < float(sw) && sy0 < float(sh) {
            def int sx = int(floor(sx0))
            def int sy = int(floor(sy0))
            _blit_pixel(dst, (py * w + px) * c, c, src, (sy * sw + sx) * sc, sc, tr, tg, tb, ta)
         }
         px += 1
      }
      py += 1
   }
   out
}

fn _utf8_char_len(int lead) int {
   if lead < 128 { return 1 }
   if lead >= 240 { return 4 }
   if lead >= 224 { return 3 }
   if lead >= 192 { return 2 }
   1
}

fn _utf8_codepoint(str text, int offset, int length) int {
   def int b0 = int(load8(text, offset)) & 255
   if length == 1 { return b0 }
   if length == 2 { return ((b0 & 31) << 6) | (int(load8(text, offset + 1)) & 63) }
   if length == 3 { return ((b0 & 15) << 12) | ((int(load8(text, offset + 1)) & 63) << 6) | (int(load8(text, offset + 2)) & 63) }
   ((b0 & 7) << 18) | ((int(load8(text, offset + 1)) & 63) << 12) | ((int(load8(text, offset + 2)) & 63) << 6) | (int(load8(text, offset + 3)) & 63)
}

fn _blend_text_pixel(any dst, int p, int channels, int r, int g, int b, int sa) any {
   if sa <= 0 { return 0 }
   if sa > 255 { sa = 255 }
   mut int sr = r
   mut int sg = g
   mut int sb = b
   if channels <= 2 {
      def int gray = (r * 299 + g * 587 + b * 114 + 500) / 1000
      sr = gray sg = gray sb = gray
   }
   def int da = channels == 2 ? int(load8(dst, p + 1)) : (channels == 4 ? int(load8(dst, p + 3)) : 255)
   def int out_a = (sa * 255 + da * (255 - sa) + 127) / 255
   if out_a <= 0 { return 0 }
   mut int k = 0
   while k < (channels == 1 || channels == 2 ? 1 : 3) {
      def int source = k == 0 ? sr : (k == 1 ? sg : sb)
      def int dest = int(load8(dst, p + k))
      mut int out = 0
      if channels == 1 || channels == 2 || channels == 3 {
         out = (source * sa + dest * (255 - sa) + 127) / 255
      } else {
         out = (source * sa * 255 + dest * da * (255 - sa) + out_a * 127) / (out_a * 255)
      }
      store8(dst, out, p + k)
      k += 1
   }
   if channels == 2 { store8(dst, out_a, p + 1) }
   if channels == 4 { store8(dst, out_a, p + 3) }
}

fn draw_text(any image, any font, str text, int x, int y, int pixel_size, list color=[255, 255, 255, 255]) any {
   "Draws TrueType text into a new image copy. Position is the top-left of the font line box; color is RGBA."
   if !_valid(image) || !is_dict(font) || !font.get("face", 0) || pixel_size <= 0 || pixel_size > 1024 || color.len < 3 || color.len > 4 || x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int channels = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def dst = out.get("data")
   def f64 scale = ttf.scale_for_pixel_height(font, float(pixel_size))
   def metrics = ttf.get_vmetrics(font)
   mut int line_height = int(float(metrics.get(2, 0.0)) * scale + 0.5)
   def int ascender = int(float(metrics.get(0, 0.0)) * scale + 0.5)
   if line_height < pixel_size { line_height = pixel_size }
   def int cr = _rgba(color, 0)
   def int cg = _rgba(color, 1)
   def int cb = _rgba(color, 2)
   def int ca = color.len == 4 ? _rgba(color, 3) : 255
   mut int pen_x = x
   mut int line_y = y
   mut int previous = 0
   mut int offset = 0
   def int text_bytes = text.len
   while offset < text_bytes {
      mut int length = _utf8_char_len(int(load8(text, offset)) & 255)
      if offset + length > text_bytes { length = text_bytes - offset }
      def int cp = _utf8_codepoint(text, offset, length)
      offset += length
      if cp == 10 {
         pen_x = x
         line_y += line_height
         previous = 0
      } elif cp != 13 {
         if cp == 9 {
            def int space_glyph = ttf.get_glyph_index(font, 32)
            def advance = ttf.get_hmetrics(font, space_glyph, pixel_size)
            pen_x += int(float(advance.get(0, 0.0)) * 4.0 + 0.5)
            previous = 0
         } else {
            mut int glyph = ttf.get_glyph_index(font, cp)
            if glyph == 0 { glyph = ttf.get_glyph_index(font, 0xfffd) }
            if glyph != 0 {
               if previous != 0 { pen_x += int(ttf.get_kern(font, previous, glyph, pixel_size) + 0.5) }
               def bitmap = ttf.get_glyph_bitmap(font, scale, scale, glyph)
               if is_dict(bitmap) {
                  def int gw = int(bitmap.get("width", 0))
                  def int gh = int(bitmap.get("height", 0))
                  def pixels = bitmap.get("data", 0)
                  if pixels && gw > 0 && gh > 0 {
                     def int left = pen_x + int(bitmap.get("xoff", 0))
                     def int top = line_y + ascender - int(bitmap.get("yoff", 0))
                     mut int gy = 0
                     while gy < gh {
                        def int dy = top + gy
                        if dy >= 0 && dy < h {
                           mut int gx = 0
                           while gx < gw {
                              def int dx = left + gx
                              if dx >= 0 && dx < w {
                                 def int coverage = int(load8(pixels, (gy * gw + gx) * 4 + 3))
                                 def int alpha = (coverage * ca + 127) / 255
                                 if alpha > 0 {
                                    def int dp = (dy * w + dx) * channels
                                    _blend_text_pixel(dst, dp, channels, cr, cg, cb, alpha)
                                 }
                              }
                              gx += 1
                           }
                        }
                        gy += 1
                     }
                  }
                  if pixels { free(pixels) }
               }
               def advance = ttf.get_hmetrics(font, glyph, pixel_size)
               pen_x += int(float(advance.get(0, 0.0)) + 0.5)
               previous = glyph
            }
         }
      }
   }
   out
}

fn _rgba(list color, int index) int {
   def int v = int(color.get(index, 0))
   v < 0 ? 0 : (v > 255 ? 255 : v)
}

fn _mix(int a, int b, f64 t) int { int(float(a) * (1.0 - t) + float(b) * t + 0.5) }

fn gradient_linear(int width, int height, int direction, list start, list end) any {
   "Generates an RGBA linear gradient. Direction is degrees clockwise from vertical, matching raylib."
   if width <= 0 || height <= 0 || start.len != 4 || end.len != 4 || direction < 0 || direction > 360 { return 0 }
   def out = create(width, height, [0, 0, 0, 0])
   if !out { return 0 }
   def f64 radians = (90.0 - float(direction)) * 3.141592653589793 / 180.0
   def f64 dx = cos(radians)
   def f64 dy = sin(radians)
   def f64 origin = 0.5 - dx * float(width) / 2.0 - dy * float(height) / 2.0
   mut f64 edge = ((dy < 0.0) == (dx < 0.0)) ? abs(origin) : abs(origin + float(width) * dx)
   if edge < 0.0000001 { edge = 1.0 }
   def data = out.get("data")
   mut int y = 0
   while y < height {
      mut int x = 0
      while x < width {
         def f64 pos = (origin + float(x) * dx + float(y) * dy) / edge
         def f64 t = (pos < -1.0 ? -1.0 : (pos > 1.0 ? 1.0 : pos)) / 2.0 + 0.5
         mut int k = 0
         while k < 4 { store8(data, _mix(_rgba(start, k), _rgba(end, k), t), (y * width + x) * 4 + k) k += 1 }
         x += 1
      }
      y += 1
   }
   out
}

fn gradient_radial(int width, int height, f64 density, list inner, list outer) any {
   "Generates an RGBA radial gradient with density in [0, 1)."
   if width <= 0 || height <= 0 || density < 0.0 || density >= 1.0 || inner.len != 4 || outer.len != 4 { return 0 }
   def out = create(width, height, [0, 0, 0, 0])
   if !out { return 0 }
   def f64 radius = float(width < height ? width : height) / 2.0
   def f64 cx = float(width) / 2.0
   def f64 cy = float(height) / 2.0
   def data = out.get("data")
   mut int y = 0
   while y < height {
      mut int x = 0
      while x < width {
         def f64 vx = float(x) - cx
         def f64 vy = float(y) - cy
         def f64 t0 = (sqrt(vx * vx + vy * vy) - radius * density) / (radius * (1.0 - density))
         def f64 t = t0 < 0.0 ? 0.0 : (t0 > 1.0 ? 1.0 : t0)
         mut int k = 0
         while k < 4 { store8(data, _mix(_rgba(inner, k), _rgba(outer, k), t), (y * width + x) * 4 + k) k += 1 }
         x += 1
      }
      y += 1
   }
   out
}

fn gradient_square(int width, int height, f64 density, list inner, list outer) any {
   "Generates an RGBA square(Chebyshev-distance) gradient with density in [0, 1)."
   if width <= 0 || height <= 0 || density < 0.0 || density >= 1.0 || inner.len != 4 || outer.len != 4 { return 0 }
   def out = create(width, height, [0, 0, 0, 0])
   if !out { return 0 }
   def f64 cx = float(width) / 2.0
   def f64 cy = float(height) / 2.0
   def data = out.get("data")
   mut int y = 0
   while y < height {
      mut int x = 0
      while x < width {
         def f64 nx = abs(float(x) - cx) / cx
         def f64 ny = abs(float(y) - cy) / cy
         def f64 t0 = ((nx > ny ? nx : ny) - density) / (1.0 - density)
         def f64 t = t0 < 0.0 ? 0.0 : (t0 > 1.0 ? 1.0 : t0)
         mut int k = 0
         while k < 4 { store8(data, _mix(_rgba(inner, k), _rgba(outer, k), t), (y * width + x) * 4 + k) k += 1 }
         x += 1
      }
      y += 1
   }
   out
}

fn rotate(any image, int degrees) any {
   "Rotates by integer degrees into the smallest axis-aligned image bounds using bilinear sampling."
   if !_valid(image) { return 0 }
   def int sw = int(image.get("width"))
   def int sh = int(image.get("height"))
   def int c = int(image.get("channels"))
   def f64 rad = float(degrees) * 3.141592653589793 / 180.0
   def f64 sn = sin(rad)
   def f64 cs = cos(rad)
   def f64 fw = abs(float(sw) * cs) + abs(float(sh) * sn)
   def f64 fh = abs(float(sh) * cs) + abs(float(sw) * sn)
   def int dw = int(fw)
   def int dh = int(fh)
   if dw <= 0 || dh <= 0 || dw > 1000000 || dh > 1000000 || dw * dh > 268435455 / c { return 0 }
   def out = create(dw, dh, c == 1 ? [0] : (c == 2 ? [0, 0] : (c == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int y = 0
   while y < dh {
      mut int x = 0
      while x < dw {
         def f64 ox0 = float(x) + 0.5 - float(dw) / 2.0
         def f64 oy0 = float(y) + 0.5 - float(dh) / 2.0
         mut f64 sx = ox0 * cs + oy0 * sn + float(sw) / 2.0 - 0.5
         mut f64 sy = oy0 * cs - ox0 * sn + float(sh) / 2.0 - 0.5
         if sx >= -0.5 && sx < float(sw) - 0.5 && sy >= -0.5 && sy < float(sh) - 0.5 {
            if sx < 0.0 { sx = 0.0 }
            if sy < 0.0 { sy = 0.0 }
            def int x0 = int(floor(sx))
            def int y0 = int(floor(sy))
            def int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1
            def int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1
            def f64 tx = sx - float(x0)
            def f64 ty = sy - float(y0)
            mut int k = 0
            while k < c {
               def int a = load8(src, (y0 * sw + x0) * c + k)
               def int b = load8(src, (y0 * sw + x1) * c + k)
               def int cc = load8(src, (y1 * sw + x0) * c + k)
               def int d = load8(src, (y1 * sw + x1) * c + k)
               def f64 value = float(a) * (1.0 - tx) * (1.0 - ty) + float(b) * tx * (1.0 - ty) + float(cc) * (1.0 - tx) * ty + float(d) * tx * ty
               store8(dst, int(value + 0.5), (y * dw + x) * c + k)
               k += 1
            }
         }
         x += 1
      }
      y += 1
   }
   out
}

fn convolve(any image, list kernel) any {
   "Applies a square floating-point kernel, repeating edge pixels outside the image."
   if !_valid(image) || kernel.len < 1 { return 0 }
   def int size = int(sqrt(float(kernel.len)) + 0.5)
   if size <= 0 || size * size != kernel.len || size > 31 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = create(w, h, c == 1 ? [0] : (c == 2 ? [0, 0] : (c == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   def int lo = -(size / 2)
   def int hi = lo + size
   mut int y = 0
   while y < h {
      mut int x = 0
      while x < w {
         mut int k = 0
         while k < c {
            mut f64 sum = 0.0
            mut int ky = lo
            while ky < hi {
               def int sy = y + ky < 0 ? 0 : (y + ky >= h ? h - 1 : y + ky)
               mut int kx = lo
               while kx < hi {
                  def int sx = x + kx < 0 ? 0 : (x + kx >= w ? w - 1 : x + kx)
                  def int ki = (ky - lo) * size + (kx - lo)
                  sum += float(load8(src, (sy * w + sx) * c + k)) * float(kernel.get(ki))
                  kx += 1
               }
               ky += 1
            }
            def int value = int(sum + 0.5)
            store8(dst, value < 0 ? 0 : (value > 255 ? 255 : value), (y * w + x) * c + k)
            k += 1
         }
         x += 1
      }
      y += 1
   }
   out
}

fn blur(any image, int radius) any {
   "Applies four horizontal/vertical box-blur passes as a Gaussian approximation."
   if !_valid(image) || radius <= 0 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def int r = radius > (w > h ? w : h) ? (w > h ? w : h) : radius
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   if c == 2 || c == 4 { out = alpha_premultiply(out) }
   def data = out.get("data")
   def scratch_img = create(w, h, c == 1 ? [0] : (c == 2 ? [0, 0] : (c == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !scratch_img { return 0 }
   def scratch = scratch_img.get("data")
   mut int pass = 0
   while pass < 4 {
      mut int y = 0
      while y < h {
         mut int k = 0
         while k < c {
            mut int sum = 0
            mut int sx = 0
            def int initial_right = r < w ? r : w - 1
            while sx <= initial_right { sum += int(load8(data, (y * w + sx) * c + k)) sx += 1 }
            mut int count = initial_right + 1
            mut int x = 0
            while x < w {
               store8(scratch, (sum + count / 2) / count, (y * w + x) * c + k)
               if x + 1 < w {
                  def int leaving = x - r
                  if leaving >= 0 { sum -= int(load8(data, (y * w + leaving) * c + k)) count -= 1 }
                  def int entering = x + r + 1
                  if entering < w { sum += int(load8(data, (y * w + entering) * c + k)) count += 1 }
               }
               x += 1
            }
            k += 1
         }
         y += 1
      }
      mut int x = 0
      while x < w {
         mut int k = 0
         while k < c {
            mut int sum = 0
            mut int sy = 0
            def int initial_bottom = r < h ? r : h - 1
            while sy <= initial_bottom { sum += int(load8(scratch, (sy * w + x) * c + k)) sy += 1 }
            mut int count = initial_bottom + 1
            mut int yv = 0
            while yv < h {
               store8(data, (sum + count / 2) / count, (yv * w + x) * c + k)
               if yv + 1 < h {
                  def int leaving = yv - r
                  if leaving >= 0 { sum -= int(load8(scratch, (leaving * w + x) * c + k)) count -= 1 }
                  def int entering = yv + r + 1
                  if entering < h { sum += int(load8(scratch, (entering * w + x) * c + k)) count += 1 }
               }
               yv += 1
            }
            k += 1
         }
         x += 1
      }
      pass += 1
   }
   if c == 2 || c == 4 {
      mut int i = 0
      while i < w * h {
         def int p = i * c
         def int a = int(load8(data, p + c - 1))
         mut int k = 0
         while k < c - 1 {
            def int premult = int(load8(data, p + k))
            def int value = a > 0 ? (premult * 255 + a / 2) / a : 0
            store8(data, value > 255 ? 255 : value, p + k)
            k += 1
         }
         i += 1
      }
   }
   out
}

fn _hash2(int x, int y, int seed, int octave=0) int {
   mut int n = (x * 374761393 + y * 668265263 + seed * 1442695041 + octave * 1013904223) % 2147483647
   if n < 0 { n = -n }
   n = (n * 48271 + 1) % 2147483647
   n
}

fn _gradient_dot(int x, int y, int seed, int octave, f64 dx, f64 dy) f64 {
   def int g = _hash2(x, y, seed, octave) % 8
   if g == 0 { return (dx + dy) * 0.7071067811865476 }
   if g == 1 { return (-dx + dy) * 0.7071067811865476 }
   if g == 2 { return (dx - dy) * 0.7071067811865476 }
   if g == 3 { return (-dx - dy) * 0.7071067811865476 }
   if g == 4 { return dx }
   if g == 5 { return -dx }
   if g == 6 { return dy }
   -dy
}

fn _fade(f64 t) f64 { t * t * t * (t * (t * 6.0 - 15.0) + 10.0) }

fn _lerp(f64 a, f64 b, f64 t) f64 { a + (b - a) * t }

fn _perlin2(f64 x, f64 y, int seed, int octave) f64 {
   def int ix = int(floor(x))
   def int iy = int(floor(y))
   def f64 fx = x - float(ix)
   def f64 fy = y - float(iy)
   def f64 u = _fade(fx)
   def f64 v = _fade(fy)
   def f64 n00 = _gradient_dot(ix, iy, seed, octave, fx, fy)
   def f64 n10 = _gradient_dot(ix + 1, iy, seed, octave, fx - 1.0, fy)
   def f64 n01 = _gradient_dot(ix, iy + 1, seed, octave, fx, fy - 1.0)
   def f64 n11 = _gradient_dot(ix + 1, iy + 1, seed, octave, fx - 1.0, fy - 1.0)
   _lerp(_lerp(n00, n10, u), _lerp(n01, n11, u), v) * 1.4142135623730951
}

fn _store_gray(data, int p, int value) any {
   def int v = value < 0 ? 0 : (value > 255 ? 255 : value)
   store8(data, v, p) store8(data, v, p + 1) store8(data, v, p + 2) store8(data, 255, p + 3)
}

fn white_noise(int width, int height, f64 factor, int seed=0) any {
   "Generates deterministic black/white RGBA noise; factor is the white-pixel probability."
   if width <= 0 || height <= 0 { return 0 }
   def out = create(width, height, [0, 0, 0, 255])
   if !out { return 0 }
   def data = out.get("data")
   mut int i = 0
   while i < width * height {
      def int x = i % width
      def int y = i / width
      def int sample = _hash2(x, y, seed) % 100
      _store_gray(data, i * 4, float(sample) < factor * 100.0 ? 255 : 0)
      i += 1
   }
   out
}

fn perlin_noise(int width, int height, int offset_x, int offset_y, f64 scale, int seed=0) any {
   "Generates six-octave deterministic Perlin FBM as a grayscale RGBA image."
   if width <= 0 || height <= 0 || scale < 0.0 { return 0 }
   def out = create(width, height, [0, 0, 0, 255])
   if !out { return 0 }
   def data = out.get("data")
   def int short_side = width < height ? width : height
   mut int y = 0
   while y < height {
      mut int x = 0
      while x < width {
         def f64 nx = float(x + offset_x) * scale / float(short_side)
         def f64 ny = float(y + offset_y) * scale / float(short_side)
         mut f64 sum = 0.0
         mut f64 frequency = 1.0
         mut f64 amplitude = 1.0
         mut int octave = 0
         while octave < 6 {
            sum += _perlin2(nx * frequency, ny * frequency, seed, octave) * amplitude
            frequency *= 2.0
            amplitude *= 0.5
            octave += 1
         }
         def f64 bounded = sum < -1.0 ? -1.0 : (sum > 1.0 ? 1.0 : sum)
         _store_gray(data, (y * width + x) * 4, int((bounded + 1.0) * 127.5))
         x += 1
      }
      y += 1
   }
   out
}

fn cellular(int width, int height, int tile_size, int seed=0) any {
   "Generates deterministic nearest-seed cellular noise with one jittered seed in each tile."
   if width <= 0 || height <= 0 || tile_size <= 0 { return 0 }
   def out = create(width, height, [255, 255, 255, 255])
   if !out { return 0 }
   def int cols = width / tile_size
   def int rows = height / tile_size
   if cols <= 0 || rows <= 0 { return out }
   def data = out.get("data")
   mut int y = 0
   while y < height {
      def int ty = y / tile_size
      mut int x = 0
      while x < width {
         def int tx = x / tile_size
         mut f64 min_d2 = 4294967296.0
         mut int sy = ty - 1
         while sy <= ty + 1 {
            if sy >= 0 && sy < rows {
               mut int sx = tx - 1
               while sx <= tx + 1 {
                  if sx >= 0 && sx < cols {
                     def int hx = _hash2(sx, sy, seed, 11) % 8
                     def int hy = _hash2(sx, sy, seed, 17) % 8
                     def f64 px = float(sx * tile_size + hx * tile_size / 8)
                     def f64 py = float(sy * tile_size + hy * tile_size / 8)
                     def f64 dx = float(x) - px
                     def f64 dy = float(y) - py
                     def f64 d2 = dx * dx + dy * dy
                     if d2 < min_d2 { min_d2 = d2 }
                  }
                  sx += 1
               }
            }
            sy += 1
         }
         def int intensity0 = int(sqrt(min_d2) * 256.0 / float(tile_size))
         _store_gray(data, (y * width + x) * 4, intensity0)
         x += 1
      }
      y += 1
   }
   out
}

fn checked(int width, int height, int cell_width, int cell_height, list first, list second) any {
   "Creates a checkerboard using cell dimensions in pixels."
   if cell_width <= 0 || cell_height <= 0 || first.len != second.len { return 0 }
   def out = create(width, height, first)
   if !out { return 0 }
   def data = out.get("data")
   def int c = first.len
   mut int y = 0
   while y < height {
      mut int x = 0
      while x < width {
         if (x / cell_width + y / cell_height) % 2 != 0 {
            mut int k = 0
            while k < c {
               def int v = int(second.get(k))
               store8(data, v < 0 ? 0 : (v > 255 ? 255 : v), (y * width + x) * c + k)
               k += 1
            }
         }
         x += 1
      }
      y += 1
   }
   out
}

fn resize(any image, int width, int height, str filter="bilinear") any {
   "Resizes packed 8-bit pixels using 'bilinear' or 'nearest'. Alpha is interpolated premultiplied for 2- and 4-channel images."
   if !_valid(image) || (filter != "bilinear" && filter != "nearest") { return 0 }
   def int sw = int(image.get("width"))
   def int sh = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = create(width, height, c == 1 ? [0] : (c == 2 ? [0, 0] : (c == 3 ? [0, 0, 0] : [0, 0, 0, 0])))
   if !out { return 0 }
   def src = image.get("data")
   def dst = out.get("data")
   mut int y = 0
   while y < height {
      mut int x = 0
      while x < width {
         if filter == "nearest" {
            def int sx = x * sw / width
            def int sy = y * sh / height
            mut int k = 0
            while k < c { store8(dst, load8(src, (sy * sw + sx) * c + k), (y * width + x) * c + k) k += 1 }
         } else {
            def f64 fx = (float(x) + 0.5) * float(sw) / float(width) - 0.5
            def f64 fy = (float(y) + 0.5) * float(sh) / float(height) - 0.5
            def int ix = int(floor(fx))
            def int iy = int(floor(fy))
            def f64 tx = fx - float(ix)
            def f64 ty = fy - float(iy)
            def int x0 = ix < 0 ? 0 : (ix >= sw ? sw - 1 : ix)
            def int x1 = ix + 1 < 0 ? 0 : (ix + 1 >= sw ? sw - 1 : ix + 1)
            def int y0 = iy < 0 ? 0 : (iy >= sh ? sh - 1 : iy)
            def int y1 = iy + 1 < 0 ? 0 : (iy + 1 >= sh ? sh - 1 : iy + 1)
            def offsets = [(y0 * sw + x0) * c, (y0 * sw + x1) * c, (y1 * sw + x0) * c, (y1 * sw + x1) * c]
            def weights = [(1.0 - tx) * (1.0 - ty), tx * (1.0 - ty), (1.0 - tx) * ty, tx * ty]
            def bool alpha = c == 2 || c == 4
            mut f64 a = 0.0
            mut int j = 0
            while j < 4 { a += float(weights.get(j)) * (alpha ? float(load8(src, int(offsets.get(j)) + c - 1)) : 255.0) j += 1 }
            mut int k = 0
            while k < c {
               mut f64 value = 0.0
               j = 0
               while j < 4 {
                  def int off = int(offsets.get(j))
                  def f64 av = alpha && k != c - 1 ? float(load8(src, off + c - 1)) : 255.0
                  value += float(weights.get(j)) * float(load8(src, off + k)) * av / 255.0
                  j += 1
               }
               if alpha && k != c - 1 { value = a > 0.0 ? value * 255.0 / a : 0.0 }
               store8(dst, int(value + 0.5), (y * width + x) * c + k)
               k += 1
            }
         }
         x += 1
      }
      y += 1
   }
   out
}

fn dither(any image, int bits) any {
   "Quantizes color channels to 1..8 bits using Floyd-Steinberg error diffusion.
   Alpha, when present, is copied unchanged. Returns a new image."
   if !_valid(image) || bits < 1 || bits > 8 { return 0 }
   def int w = int(image.get("width"))
   def int h = int(image.get("height"))
   def int c = int(image.get("channels"))
   def out = crop(image, 0, 0, w, h)
   if !out { return 0 }
   def data = out.get("data")
   def int colors = (c == 2 || c == 4) ? c - 1 : c
   def int levels = (1 << bits) - 1
   ;; Two scanlines suffice; padding avoids special array bounds at the edges.
   mut int k = 0
   while k < colors {
      mut current = []
      mut next = []
      mut int i = 0
      while i < w + 2 { current = current.append(0.0) next = next.append(0.0) i += 1 }
      mut int y = 0
      while y < h {
         mut int x = 0
         while x < w {
            def int pos = (y * w + x) * c + k
            def int center = int(x + 1)
            def int right = int(x + 2)
            mut f64 v = float(load8(data, pos)) + float(current.get(center))
            if v < 0.0 { v = 0.0 } elif v > 255.0 { v = 255.0 }
            def int q = int(v * float(levels) / 255.0 + 0.5)
            def int result = int(float(q) * 255.0 / float(levels) + 0.5)
            store8(data, result, pos)
            def f64 error = v - float(result)
            current[right] = float(current.get(right)) + error * 7.0 / 16.0
            next[x] = float(next.get(x)) + error * 3.0 / 16.0
            next[center] = float(next.get(center)) + error * 5.0 / 16.0
            next[right] = float(next.get(right)) + error / 16.0
            x += 1
         }
         current = next
         next = []
         i = 0
         while i < w + 2 { next = next.append(0.0) i += 1 }
         y += 1
      }
      k += 1
   }
   out
}
